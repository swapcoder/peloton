//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// stats_aggregator.cpp
//
// Identification: src/statistics/stats_aggregator.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <cassert>
#include <condition_variable>
#include <fstream>
#include <memory>

#include "catalog/catalog.h"
#include "catalog/catalog_util.h"
#include "statistics/backend_stats_context.h"
#include "statistics/stats_aggregator.h"

namespace peloton {
namespace stats {

StatsAggregator::StatsAggregator(int64_t aggregation_interval_ms)
    : stats_history_(0, false),
      aggregated_stats_(LATENCY_MAX_HISTORY_AGGREGATOR, false),
      aggregation_interval_ms_(aggregation_interval_ms),
      thread_number_(0),
      total_prev_txn_committed_(0) {
  pool_.reset(new common::VarlenPool());
  try {
    ofs_.open(peloton_stats_directory_, std::ofstream::out);
  } catch (std::ofstream::failure &e) {
    LOG_ERROR("Couldn't open the stats log file %s", e.what());
  }
  LaunchAggregator();
}

StatsAggregator::~StatsAggregator() {
  LOG_DEBUG("StatsAggregator destruction");
  ShutdownAggregator();
  try {
    ofs_.close();
  } catch (std::ofstream::failure &e) {
    LOG_ERROR("Couldn't close the stats log file %s", e.what());
  }
}

void StatsAggregator::LaunchAggregator() {
  if (!is_aggregating_) {
    aggregator_thread_ = std::thread(&StatsAggregator::RunAggregator, this);
    is_aggregating_ = true;
  }
}

void StatsAggregator::ShutdownAggregator() {
  if (is_aggregating_) {
    is_aggregating_ = false;
    exec_finished_.notify_one();
    LOG_DEBUG("notifying aggregator thread...");
    aggregator_thread_.join();
    LOG_DEBUG("aggregator thread joined");
  }
}

void StatsAggregator::Aggregate(int64_t &interval_cnt, double &alpha,
                                double &weighted_avg_throughput) {
  interval_cnt++;
  LOG_TRACE(
      "\n//////////////////////////////////////////////////////"
      "//////////////////////////////////////////////////////\n");
  LOG_TRACE("TIME ELAPSED: %ld sec", interval_cnt);

  aggregated_stats_.Reset();
  std::thread::id this_id = aggregator_thread_.get_id();

  for (auto &val : backend_stats_) {
    // Exclude the txn stats generated by the aggregator thread
    if (val.first != this_id) {
      aggregated_stats_.Aggregate((*val.second));
    }
  }
  aggregated_stats_.Aggregate(stats_history_);
  LOG_TRACE("%s\n", aggregated_stats_.ToString().c_str());

  int64_t current_txns_committed = 0;
  // Traverse the metric of all threads to get the total number of committed
  // txns.
  for (auto &database_item : aggregated_stats_.database_metrics_) {
    current_txns_committed += database_item.second->GetTxnCommitted().GetCounter();
  }
  int64_t txns_committed_this_interval = current_txns_committed - total_prev_txn_committed_;
  double throughput_ = (double)txns_committed_this_interval / 1000 * STATS_AGGREGATION_INTERVAL_MS;
  double avg_throughput_ =
      (double)current_txns_committed / interval_cnt / STATS_AGGREGATION_INTERVAL_MS * 1000;
  if (interval_cnt == 1) {
    weighted_avg_throughput = throughput_;
  } else {
    weighted_avg_throughput = alpha * throughput_ + (1 - alpha) * weighted_avg_throughput;
  }

  total_prev_txn_committed_ = current_txns_committed;
  LOG_TRACE("Average throughput:     %lf txn/s", avg_throughput_);
  LOG_TRACE("Moving avg. throughput: %lf txn/s", weighted_avg_throughput);
  LOG_TRACE("Current throughput:     %lf txn/s", throughput_);

  // Write the stats to metric tables
  UpdateMetrics();

  if (interval_cnt % STATS_LOG_INTERVALS == 0) {
    try {
      ofs_ << "At interval: " << interval_cnt << std::endl;
      ofs_ << aggregated_stats_.ToString();
      ofs_ << "Weighted avg. throughput=" << weighted_avg_throughput << std::endl;
      ofs_ << "Average throughput=" << avg_throughput_ << std::endl;
      ofs_ << "Current throughput=" << throughput_;
    } catch (std::ofstream::failure &e) {
      LOG_ERROR("Error when writing to the stats log file %s", e.what());
    }
  }
}

void StatsAggregator::UpdateQueryMetrics(int64_t time_stamp, concurrency::Transaction *txn) {
  // Get the target query metrics table
  LOG_TRACE("Inserting Query Metric Tuples");
  auto query_metrics_table = GetMetricTable(QUERY_METRIC_NAME);

  std::shared_ptr<QueryMetric> query_metric;
  auto &completed_query_metrics = aggregated_stats_.GetCompletedQueryMetrics();
  while (completed_query_metrics.Dequeue(query_metric)) {
    // Get physical stats
    auto table_access = query_metric->GetQueryAccess();
    auto reads = table_access.GetReads();
    auto updates = table_access.GetUpdates();
    auto deletes = table_access.GetDeletes();
    auto inserts = table_access.GetInserts();
    auto latency = query_metric->GetQueryLatency().GetFirstLatencyValue();
    auto cpu_system = query_metric->GetProcessorMetric().GetSystemDuration();
    auto cpu_user = query_metric->GetProcessorMetric().GetUserDuration();

    // Get query params
    auto query_params = query_metric->GetQueryParams();
    auto num_params = 0;
    QueryMetric::QueryParamBuf value_buf;
    QueryMetric::QueryParamBuf type_buf;
    QueryMetric::QueryParamBuf format_buf;

    if (query_params != nullptr) {
      value_buf = query_params->val_buf_copy;
      num_params = query_params->num_params;
      format_buf = query_params->format_buf_copy;
      type_buf = query_params->type_buf_copy;
      PL_ASSERT(num_params > 0);
    }

    // Generate and insert the tuple
    auto query_tuple = catalog::GetQueryMetricsCatalogTuple(
        query_metrics_table->GetSchema(), query_metric->GetName(), query_metric->GetDatabaseId(),
        num_params, type_buf, format_buf, value_buf, reads, updates, deletes, inserts,
        (int64_t)latency, (int64_t)(cpu_system + cpu_user), time_stamp, pool_.get());
    catalog::InsertTuple(query_metrics_table, std::move(query_tuple), txn);
    LOG_TRACE("Query Metric Tuple inserted");
  }
}

void StatsAggregator::UpdateMetrics() {
  // All tuples are inserted in a single txn
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();
  auto txn = txn_manager.BeginTransaction();

  // Get the target table metrics table
  LOG_TRACE("Inserting stat tuples into catalog database..");
  auto catalog = catalog::Catalog::GetInstance();
  auto database_metrics_table = GetMetricTable(DATABASE_METRIC_NAME);

  auto time_since_epoch = std::chrono::system_clock::now().time_since_epoch();
  auto time_stamp = std::chrono::duration_cast<std::chrono::seconds>(time_since_epoch).count();

  auto database_count = catalog->GetDatabaseCount();
  for (oid_t database_offset = 0; database_offset < database_count; database_offset++) {
    auto database = catalog->GetDatabaseWithOffset(database_offset);

    // Update database metrics table
    auto database_oid = database->GetOid();
    auto database_metric = aggregated_stats_.GetDatabaseMetric(database_oid);
    auto txn_committed = database_metric->GetTxnCommitted().GetCounter();
    auto txn_aborted = database_metric->GetTxnAborted().GetCounter();

    auto db_tuple = catalog::GetDatabaseMetricsCatalogTuple(
        database_metrics_table->GetSchema(), database_oid, txn_committed, txn_aborted, time_stamp);

    catalog::InsertTuple(database_metrics_table, std::move(db_tuple), txn);
    LOG_TRACE("DB Metric Tuple inserted");

    // Update all the indices of this database
    UpdateTableMetrics(database, time_stamp, txn);
  }

  // Update all query metrics
  UpdateQueryMetrics(time_stamp, txn);

  txn_manager.CommitTransaction(txn);
}

void StatsAggregator::UpdateTableMetrics(storage::Database *database, int64_t time_stamp,
                                         concurrency::Transaction *txn) {
  // Get the target table metrics table
  auto database_oid = database->GetOid();
  auto table_metrics_table = GetMetricTable(TABLE_METRIC_NAME);

  // Update table metrics table for each of the indices
  auto table_count = database->GetTableCount();
  for (oid_t table_offset = 0; table_offset < table_count; table_offset++) {
    auto table = database->GetTable(table_offset);
    auto table_oid = table->GetOid();
    auto table_metrics = aggregated_stats_.GetTableMetric(database_oid, table_oid);
    auto table_access = table_metrics->GetTableAccess();
    auto reads = table_access.GetReads();
    auto updates = table_access.GetUpdates();
    auto deletes = table_access.GetDeletes();
    auto inserts = table_access.GetInserts();

    // Generate and insert the tuple
    auto table_tuple = catalog::GetTableMetricsCatalogTuple(table_metrics_table->GetSchema(),
                                                            database_oid, table_oid, reads, updates,
                                                            deletes, inserts, time_stamp);
    catalog::InsertTuple(table_metrics_table, std::move(table_tuple), txn);
    LOG_TRACE("Table Metric Tuple inserted");

    UpdateIndexMetrics(database, table, time_stamp, txn);
  }
}

void StatsAggregator::UpdateIndexMetrics(storage::Database *database, storage::DataTable *table,
                                         int64_t time_stamp, concurrency::Transaction *txn) {
  // Get the target index metrics table
  auto index_metrics_table = GetMetricTable(INDEX_METRIC_NAME);

  // Update index metrics table for each of the indices
  auto database_oid = database->GetOid();
  auto table_oid = table->GetOid();
  auto index_count = table->GetIndexCount();
  for (oid_t index_offset = 0; index_offset < index_count; index_offset++) {
    auto index = table->GetIndex(index_offset);
    auto index_oid = index->GetOid();
    auto index_metric = aggregated_stats_.GetIndexMetric(database_oid, table_oid, index_oid);

    auto index_access = index_metric->GetIndexAccess();
    auto reads = index_access.GetReads();
    auto deletes = index_access.GetDeletes();
    auto inserts = index_access.GetInserts();

    // Generate and insert the tuple
    auto index_tuple = catalog::GetIndexMetricsCatalogTuple(index_metrics_table->GetSchema(),
                                                            database_oid, table_oid, index_oid,
                                                            reads, deletes, inserts, time_stamp);

    catalog::InsertTuple(index_metrics_table, std::move(index_tuple), txn);
  }
}

void StatsAggregator::RunAggregator() {
  LOG_DEBUG("Aggregator is now running.");
  std::mutex mtx;
  std::unique_lock<std::mutex> lck(mtx);
  int64_t interval_cnt = 0;
  double alpha = 0.4;
  double weighted_avg_throughput = 0.0;

  while (exec_finished_.wait_for(lck, std::chrono::milliseconds(aggregation_interval_ms_)) ==
             std::cv_status::timeout &&
         is_aggregating_) {
    Aggregate(interval_cnt, alpha, weighted_avg_throughput);
  }
  LOG_DEBUG("Aggregator done!");
}

StatsAggregator &StatsAggregator::GetInstance(int64_t aggregation_interval_ms) {
  static StatsAggregator stats_aggregator(aggregation_interval_ms);
  return stats_aggregator;
}

//===--------------------------------------------------------------------===//
// HELPER FUNCTIONS
//===--------------------------------------------------------------------===//

// Register the BackendStatsContext of a worker thread to global Stats
// Aggregator
void StatsAggregator::RegisterContext(std::thread::id id_, BackendStatsContext *context_) {
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    PL_ASSERT(backend_stats_.find(id_) == backend_stats_.end());

    thread_number_++;
    backend_stats_[id_] = context_;
  }
  LOG_DEBUG("Stats aggregator hash map size: %ld", backend_stats_.size());
}

// Unregister a BackendStatsContext. Currently we directly reuse the thread id
// instead of explicitly unregistering it.
void StatsAggregator::UnregisterContext(std::thread::id id) {
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    if (backend_stats_.find(id) != backend_stats_.end()) {
      stats_history_.Aggregate(*backend_stats_[id]);
      backend_stats_.erase(id);
      thread_number_--;
    } else {
      LOG_DEBUG("stats_context already deleted!");
    }
  }
}

storage::DataTable *StatsAggregator::GetMetricTable(std::string table_name) {
  auto catalog = catalog::Catalog::GetInstance();
  PL_ASSERT(catalog->GetDatabaseCount() > 0);
  storage::Database *catalog_database = catalog->GetDatabaseWithName(CATALOG_DATABASE_NAME);
  PL_ASSERT(catalog_database != nullptr);
  auto metrics_table = catalog_database->GetTableWithName(table_name);
  PL_ASSERT(metrics_table != nullptr);
  return metrics_table;
}

}  // namespace stats
}  // namespace peloton
