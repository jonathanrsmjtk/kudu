// Copyright (c) 2013, Cloudera, inc.

#include <boost/assign/list_of.hpp>
#include <boost/foreach.hpp>
#include <gtest/gtest.h>
#include <gflags/gflags.h>

#include <tr1/memory>
#include <vector>

#include "client/client.h"
#include "common/row.h"
#include "common/wire_protocol.h"
#include "gutil/stl_util.h"
#include "integration-tests/mini_cluster.h"
#include "master/master-test-util.h"
#include "master/mini_master.h"
#include "tablet/tablet_peer.h"
#include "tablet/transactions/write_transaction.h"
#include "tserver/mini_tablet_server.h"
#include "tserver/scanners.h"
#include "tserver/tablet_server.h"
#include "tserver/ts_tablet_manager.h"
#include "util/net/sockaddr.h"
#include "util/status.h"
#include "util/stopwatch.h"
#include "util/test_util.h"

DECLARE_int32(heartbeat_interval_ms);
DEFINE_int32(test_scan_num_rows, 1000, "Number of rows to insert and scan");

using std::tr1::shared_ptr;

namespace kudu {
namespace client {

using tablet::TabletPeer;
using tserver::MiniTabletServer;
using tserver::ColumnRangePredicatePB;

class ClientTest : public KuduTest {
 public:
  ClientTest()
    : schema_(boost::assign::list_of
              (ColumnSchema("key", UINT32))
              (ColumnSchema("int_val", UINT32))
              (ColumnSchema("string_val", STRING, true)),
              1),
      rb_(schema_) {
  }

  virtual void SetUp() {
    KuduTest::SetUp();

    FLAGS_heartbeat_interval_ms = 10;

    // Start minicluster
    cluster_.reset(new MiniCluster(env_.get(), test_dir_, 1));
    ASSERT_STATUS_OK(cluster_->Start());

    // Set up two test tablets inside the server.
    ASSERT_NO_FATAL_FAILURE(CreateTabletForTesting(
                              cluster_->mini_master(), "fake-table", schema_, &tablet_id_));
    ASSERT_NO_FATAL_FAILURE(CreateTabletForTesting(
                              cluster_->mini_master(), "fake-table-2", schema_, &tablet2_id_));

    // Grab a reference to the first of them, for more invasive testing.
    ASSERT_TRUE(cluster_->mini_tablet_server(0)->server()->tablet_manager()->LookupTablet(
                tablet_id_, &tablet_peer_));

    // Wait for the tablets to be reported to the master.
    ASSERT_STATUS_OK(cluster_->WaitForReplicaCount(tablet_id_, 1));
    ASSERT_STATUS_OK(cluster_->WaitForReplicaCount(tablet2_id_, 1));

    // Connect to the cluster.
    KuduClientOptions opts;
    opts.master_server_addr = cluster_->mini_master()->bound_rpc_addr().ToString();
    ASSERT_STATUS_OK(KuduClient::Create(opts, &client_));
    ASSERT_STATUS_OK(client_->OpenTable(tablet_id_, schema_, &client_table_));
    ASSERT_STATUS_OK(client_->OpenTable(tablet2_id_, schema_, &client_table2_));
  }

 protected:

  string tablet_id_;
  string tablet2_id_;

  // Inserts 'num_rows' test rows directly into the tablet (i.e not via RPC)
  void InsertTestRows(int num_rows) {
    tablet::WriteTransactionContext tx_ctx;
    for (int i = 0; i < num_rows; i++) {
      CHECK_OK(tablet_peer_->tablet()->Insert(&tx_ctx, BuildTestRow(i)));
      tx_ctx.Reset();
    }
    CHECK_OK(tablet_peer_->tablet()->Flush());
  }

  ConstContiguousRow BuildTestRow(int index) {
    rb_.Reset();
    rb_.AddUint32(index);
    rb_.AddUint32(index * 2);
    rb_.AddString(StringPrintf("hello %d", index));
    return rb_.row();
  }

  void DoTestScanWithoutPredicates() {
    Schema projection = schema_.CreateKeyProjection();
    KuduScanner scanner(client_table_.get());
    ASSERT_STATUS_OK(scanner.SetProjection(projection));
    LOG_TIMING(INFO, "Scanning with no predicates") {
      ASSERT_STATUS_OK(scanner.Open());

      ASSERT_TRUE(scanner.HasMoreRows());
      vector<const uint8_t*> rows;
      uint64_t sum = 0;
      while (scanner.HasMoreRows()) {
        ASSERT_STATUS_OK(scanner.NextBatch(&rows));

        BOOST_FOREACH(const uint8_t* row_ptr, rows) {
          ConstContiguousRow row(projection, row_ptr);
          uint32_t to_add = *projection.ExtractColumnFromRow<UINT32>(row, 0);
          sum += implicit_cast<uint64_t>(to_add);
        }
      }
      // The sum should be the sum of the arithmetic series from
      // 0..FLAGS_test_scan_num_rows-1
      uint64_t expected = implicit_cast<uint64_t>(FLAGS_test_scan_num_rows) *
                            (0 + (FLAGS_test_scan_num_rows - 1)) / 2;
      ASSERT_EQ(expected, sum);
    }
  }

  void DoTestScanWithStringPredicate() {
    KuduScanner scanner(client_table_.get());
    ASSERT_STATUS_OK(scanner.SetProjection(schema_));
    ColumnRangePredicatePB pred;
    ColumnSchemaToPB(schema_.column(2), pred.mutable_column());
    pred.set_lower_bound("hello 2");
    pred.set_upper_bound("hello 3");
    ASSERT_STATUS_OK(scanner.AddConjunctPredicate(pred));

    LOG_TIMING(INFO, "Scanning with no predicates") {
      ASSERT_STATUS_OK(scanner.Open());

      ASSERT_TRUE(scanner.HasMoreRows());
      vector<const uint8_t*> rows;
      while (scanner.HasMoreRows()) {
        ASSERT_STATUS_OK(scanner.NextBatch(&rows));

        BOOST_FOREACH(const uint8_t* row_ptr, rows) {
          ConstContiguousRow row(schema_, row_ptr);
          Slice s = *schema_.ExtractColumnFromRow<STRING>(row, 2);
          if (!s.starts_with("hello 2") && s != Slice("hello 3")) {
            FAIL() << schema_.DebugRow(row);
          }
        }
      }
    }
  }

  int CountRowsFromClient(KuduTable* table) {
    KuduScanner scanner(table);
    CHECK_OK(scanner.SetProjection(table->schema()));
    CHECK_OK(scanner.Open());
    int count = 0;
    vector<const uint8_t*> rows;
    while (scanner.HasMoreRows()) {
      CHECK_OK(scanner.NextBatch(&rows));
      count += rows.size();
    }
    return count;
  }

  enum WhichServerToKill {
    DEAD_MASTER,
    DEAD_TSERVER
  };
  void DoTestWriteWithDeadServer(WhichServerToKill which);

  Schema schema_;
  RowBuilder rb_;

  gscoped_ptr<MiniCluster> cluster_;
  shared_ptr<KuduClient> client_;
  scoped_refptr<KuduTable> client_table_;
  scoped_refptr<KuduTable> client_table2_;
  shared_ptr<TabletPeer> tablet_peer_;
};

// DISABLED: OpenTable doesn't currently do an RPC.
// TODO: re-enable this once OpenTable is doing an RPC to get the Schema
// info.
TEST_F(ClientTest, DISABLED_TestBadTable) {
  scoped_refptr<KuduTable> t;
  Status s = client_->OpenTable("xxx-does-not-exist", Schema(), &t);
  ASSERT_EQ("Not found: No replicas for tablet xxx-does-not-exist", s.ToString());
}

// Test that, if the master is down, we get an appropriate error
// message.
// DISABLED: OpenTable doesn't currently do an RPC.
// TODO: re-enable this once OpenTable is doing an RPC to get the Schema
// info.
TEST_F(ClientTest, DISABLED_TestMasterDown) {
  ASSERT_STATUS_OK(cluster_->mini_master()->Shutdown());
  scoped_refptr<KuduTable> t;
  Status s = client_->OpenTable("other-tablet", Schema(), &t);
  ASSERT_TRUE(s.IsNetworkError());
  ASSERT_STR_CONTAINS(s.ToString(), "Connection refused");
}

TEST_F(ClientTest, TestScan) {
  InsertTestRows(FLAGS_test_scan_num_rows);

  DoTestScanWithoutPredicates();
  DoTestScanWithStringPredicate();
}

TEST_F(ClientTest, TestScanEmptyTable) {
  KuduScanner scanner(client_table_.get());
  ASSERT_STATUS_OK(scanner.Open());
  ASSERT_FALSE(scanner.HasMoreRows());
  scanner.Close();
}

// Test scanning with an empty projection. This should yield an empty
// row block with the proper number of rows filled in. Impala issues
// scans like this in order to implement COUNT(*).
TEST_F(ClientTest, TestScanEmptyProjection) {
  InsertTestRows(FLAGS_test_scan_num_rows);
  Schema empty_projection(vector<ColumnSchema>(), 0);
  KuduScanner scanner(client_table_.get());
  ASSERT_STATUS_OK(scanner.SetProjection(empty_projection));
  LOG_TIMING(INFO, "Scanning with no projected columns") {
    ASSERT_STATUS_OK(scanner.Open());

    ASSERT_TRUE(scanner.HasMoreRows());
    vector<const uint8_t*> rows;
    uint64_t count = 0;
    while (scanner.HasMoreRows()) {
      ASSERT_STATUS_OK(scanner.NextBatch(&rows));
      count += rows.size();
    }
    ASSERT_EQ(FLAGS_test_scan_num_rows, count);
  }
}

static void AssertScannersDisappear(const tserver::ScannerManager* manager) {
  // The Close call is async, so we may have to loop a bit until we see it disappear.
  // This loops for ~10sec. Typically it succeeds in only a few milliseconds.
  int i = 0;
  for (i = 0; i < 500; i++) {
    if (manager->CountActiveScanners() == 0) {
      LOG(INFO) << "Successfully saw scanner close on iteration " << i;
      return;
    }
    // Sleep 2ms on first few times through, then longer on later iterations.
    usleep(i < 10 ? 2000 : 20000);
  }
  FAIL() << "Waited too long for the scanner to close";
}

// Test cleanup of scanners on the server side when closed.
TEST_F(ClientTest, TestCloseScanner) {
  InsertTestRows(10);

  const tserver::ScannerManager* manager =
    cluster_->mini_tablet_server(0)->server()->scanner_manager();
  // Open the scanner, make sure it gets closed right away
  {
    SCOPED_TRACE("Implicit close");
    KuduScanner scanner(client_table_.get());
    ASSERT_STATUS_OK(scanner.SetProjection(schema_));
    ASSERT_STATUS_OK(scanner.Open());
    ASSERT_EQ(0, manager->CountActiveScanners());
    scanner.Close();
    AssertScannersDisappear(manager);
  }

  // Open the scanner, make sure we see 1 registered scanner.
  {
    SCOPED_TRACE("Explicit close");
    KuduScanner scanner(client_table_.get());
    ASSERT_STATUS_OK(scanner.SetProjection(schema_));
    ASSERT_STATUS_OK(scanner.SetBatchSizeBytes(0)); // won't return data on open
    ASSERT_STATUS_OK(scanner.Open());
    ASSERT_EQ(1, manager->CountActiveScanners());
    scanner.Close();
    AssertScannersDisappear(manager);
  }

  {
    SCOPED_TRACE("Close when out of scope");
    {
      KuduScanner scanner(client_table_.get());
      ASSERT_STATUS_OK(scanner.SetProjection(schema_));
      ASSERT_STATUS_OK(scanner.SetBatchSizeBytes(0));
      ASSERT_STATUS_OK(scanner.Open());
      ASSERT_EQ(1, manager->CountActiveScanners());
    }
    // Above scanner went out of scope, so the destructor should close asynchronously.
    AssertScannersDisappear(manager);
  }
}

// Simplest case of inserting through the client API: a single row
// with manual batching.
TEST_F(ClientTest, TestInsertSingleRowManualBatch) {
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_FALSE(session->HasPendingOperations());

  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));

  gscoped_ptr<Insert> insert = client_table_->NewInsert();
  // Try inserting without specifying a key: should fail.
  ASSERT_STATUS_OK(insert->mutable_row()->SetUInt32("int_val", 54321));
  ASSERT_STATUS_OK(insert->mutable_row()->SetStringCopy("string_val", "hello world"));

  Status s = session->Apply(&insert);
  ASSERT_EQ("Illegal state: Key not specified: "
            "INSERT uint32 int_val=54321, string string_val=hello world",
            s.ToString());

  ASSERT_STATUS_OK(insert->mutable_row()->SetUInt32("key", 12345));
  ASSERT_STATUS_OK(session->Apply(&insert));
  ASSERT_TRUE(insert == NULL) << "Successful insert should take ownership";
  ASSERT_TRUE(session->HasPendingOperations()) << "Should be pending until we Flush";

  ASSERT_STATUS_OK(session->Flush());
}

static Status ApplyInsertToSession(KuduSession* session,
                                   const scoped_refptr<KuduTable>& table,
                                   int row_key,
                                   int int_val,
                                   const char* string_val) {
  gscoped_ptr<Insert> insert = table->NewInsert();
  RETURN_NOT_OK(insert->mutable_row()->SetUInt32("key", row_key));
  RETURN_NOT_OK(insert->mutable_row()->SetUInt32("int_val", int_val));
  RETURN_NOT_OK(insert->mutable_row()->SetStringCopy("string_val", string_val));
  return session->Apply(&insert);
}

// Test which does an async flush and then drops the reference
// to the Session. This should still call the callback.
TEST_F(ClientTest, TestAsyncFlushResponseAfterSessionDropped) {
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  ASSERT_STATUS_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  Synchronizer s;
  session->FlushAsync(s.callback());
  session.reset();
  ASSERT_STATUS_OK(s.Wait());

  // Try again, this time with an error response (trying to re-insert the same row).
  s.Reset();
  session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  ASSERT_STATUS_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "row"));
  ASSERT_EQ(1, session->CountBufferedOperations());
  session->FlushAsync(s.callback());
  ASSERT_EQ(0, session->CountBufferedOperations());
  session.reset();
  ASSERT_FALSE(s.Wait().ok());
}

// Test which sends multiple batches through the same session, each of which
// contains multiple rows spread across multiple tablets.
TEST_F(ClientTest, TestMultipleMultiRowManualBatches) {
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));

  const int kNumBatches = 5;
  const int kRowsPerBatch = 10;

  int row_key = 0;

  for (int batch_num = 0; batch_num < kNumBatches; batch_num++) {
    for (int i = 0; i < kRowsPerBatch; i++) {
      ASSERT_STATUS_OK(ApplyInsertToSession(
                         session.get(),
                         (row_key % 2 == 0) ? client_table_ : client_table2_,
                         row_key, row_key * 10, "hello world"));
      row_key++;
    }
    ASSERT_TRUE(session->HasPendingOperations()) << "Should be pending until we Flush";
    ASSERT_STATUS_OK(session->Flush());
    ASSERT_FALSE(session->HasPendingOperations()) << "Should have no more pending ops after flush";
  }

  const int kNumRowsPerTablet = kNumBatches * kRowsPerBatch / 2;
  ASSERT_EQ(kNumRowsPerTablet, CountRowsFromClient(client_table_.get()));
  ASSERT_EQ(kNumRowsPerTablet, CountRowsFromClient(client_table2_.get()));
}

// Test a batch where one of the inserted rows succeeds while another
// fails.
TEST_F(ClientTest, TestBatchWithPartialError) {
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));

  // Insert a row with key "1"
  ASSERT_STATUS_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "original row"));
  ASSERT_STATUS_OK(session->Flush());

  // Now make a batch that has key "1" (which will fail) along with
  // key "2" which will succeed. Flushing should return an error.
  ASSERT_STATUS_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "Attempted dup"));
  ASSERT_STATUS_OK(ApplyInsertToSession(session.get(), client_table_, 2, 1, "Should succeed"));
  Status s = session->Flush();
  ASSERT_FALSE(s.ok());
  ASSERT_STR_CONTAINS(s.ToString(), "Some errors occurred");

  // Fetch and verify the reported error.
  ASSERT_EQ(1, session->CountPendingErrors());
  vector<Error*> errors;
  ElementDeleter d(&errors);
  bool overflow;
  session->GetPendingErrors(&errors, &overflow);
  ASSERT_FALSE(overflow);
  ASSERT_EQ(1, errors.size());
  ASSERT_TRUE(errors[0]->status().IsAlreadyPresent());
  ASSERT_EQ(errors[0]->failed_op().ToString(),
            "INSERT uint32 key=1, uint32 int_val=1, string string_val=Attempted dup");
}

// Test flushing an empty batch (should be a no-op).
TEST_F(ClientTest, TestEmptyBatch) {
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  ASSERT_STATUS_OK(session->Flush());
}

void ClientTest::DoTestWriteWithDeadServer(WhichServerToKill which) {
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));

  // Shut down the server.
  switch (which) {
    case DEAD_MASTER:
      ASSERT_STATUS_OK(cluster_->mini_master()->Shutdown());
      break;
    case DEAD_TSERVER:
      ASSERT_STATUS_OK(cluster_->mini_tablet_server(0)->Shutdown());
      break;
  }

  // Try a write.
  ASSERT_STATUS_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "x"));
  Status s = session->Flush();
  ASSERT_TRUE(s.IsIOError()) << s.ToString();
  ASSERT_EQ(1, session->CountPendingErrors());

  vector<Error*> errors;
  ElementDeleter d(&errors);
  bool overflow;
  session->GetPendingErrors(&errors, &overflow);
  ASSERT_FALSE(overflow);
  ASSERT_EQ(1, errors.size());
  ASSERT_TRUE(errors[0]->status().IsNetworkError());
  ASSERT_EQ(errors[0]->failed_op().ToString(),
            "INSERT uint32 key=1, uint32 int_val=1, string string_val=x");
}

// Test error handling cases where the master is down (tablet resolution fails)
TEST_F(ClientTest, TestWriteWithDeadMaster) {
  DoTestWriteWithDeadServer(DEAD_MASTER);
}

// Test error handling when the TS is down (actual write fails its RPC)
TEST_F(ClientTest, TestWriteWithDeadTabletServer) {
  DoTestWriteWithDeadServer(DEAD_TSERVER);
}

// Applies some updates to the session, and then drops the reference to the
// Session before flushing. Makes sure that the tablet resolution callbacks
// properly deal with the session disappearing underneath.
TEST_F(ClientTest, TestApplyToSessionWithoutFlushing) {
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  ASSERT_STATUS_OK(ApplyInsertToSession(session.get(), client_table_, 1, 1, "x"));
  session.reset(); // should not crash!
}

// Do a write with a bad schema on the client side. This should make the Prepare
// phase of the write fail, which will result in an error on the RPC response.
// This is currently disabled since it caught a bug (KUDU-72).
TEST_F(ClientTest, TestWriteWithBadSchema) {
  // Client uses an incompatible schema ('bad_col' doesn't exist on the server)
  Schema bad_schema(boost::assign::list_of
                    (ColumnSchema("key", UINT32))
                    (ColumnSchema("bad_col", UINT32)),
                    1);
  // This succeeds since we don't actually verify the schema on Open, currently.
  scoped_refptr<KuduTable> table;
  ASSERT_STATUS_OK(client_->OpenTable(tablet_id_, bad_schema, &table));

  // Try to do a write with the bad schema.
  shared_ptr<KuduSession> session = client_->NewSession();
  ASSERT_STATUS_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  gscoped_ptr<Insert> insert = table->NewInsert();
  ASSERT_STATUS_OK(insert->mutable_row()->SetUInt32("key", 12345));
  ASSERT_STATUS_OK(insert->mutable_row()->SetUInt32("bad_col", 12345));
  ASSERT_STATUS_OK(session->Apply(&insert));
  Status s = session->Flush();
  ASSERT_FALSE(s.ok());

  // Verify the specific error.
  vector<Error*> errors;
  ElementDeleter d(&errors);
  bool overflow;
  session->GetPendingErrors(&errors, &overflow);
  ASSERT_FALSE(overflow);
  ASSERT_EQ(1, errors.size());
  ASSERT_TRUE(errors[0]->status().IsInvalidArgument());
  ASSERT_EQ(errors[0]->status().ToString(),
            "Invalid argument: Some columns are not present in the current schema: bad_col");
  ASSERT_EQ(errors[0]->failed_op().ToString(),
            "INSERT uint32 key=12345, uint32 bad_col=12345");
}

} // namespace client
} // namespace kudu
