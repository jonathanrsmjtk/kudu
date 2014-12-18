// Copyright (c) 2014, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include "kudu/client/table-internal.h"

#include <string>

#include "kudu/client/client-internal.h"
#include "kudu/common/wire_protocol.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/master.proxy.h"
#include "kudu/rpc/rpc_controller.h"
#include "kudu/util/monotime.h"

namespace kudu {

using master::GetTableLocationsRequestPB;
using master::GetTableLocationsResponsePB;
using std::string;
using std::tr1::shared_ptr;

namespace client {

KuduTable::Data::Data(const shared_ptr<KuduClient>& client,
                      const string& name,
                      const KuduSchema& schema)
  : client_(client),
    name_(name),
    schema_(schema) {
}

KuduTable::Data::~Data() {
}

Status KuduTable::Data::Open() {
  // TODO: fetch the schema from the master here once catalog is available.
  GetTableLocationsRequestPB req;
  GetTableLocationsResponsePB resp;

  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(client_->data_->default_select_master_timeout_);
  req.mutable_table()->set_table_name(name_);
  Status s;
  // TODO: replace this with Async RPC-retrier based RPC in the next revision,
  // adding exponential backoff and allowing this to be used safely in a
  // a reactor thread.
  while (true) {
    if (deadline.ComesBefore(MonoTime::Now(MonoTime::FINE))) {
      // TODO: See KUDU-572, regarding better design and/or documentation for
      // timeouts and failure detection.
      int64_t timeout_millis =
          client_->data_->default_select_master_timeout_.ToMilliseconds();
      LOG(ERROR) << "Timed out waiting for non-empty GetTableLocations reply from a leader Master "
          "after " << timeout_millis << " ms.";
      return Status::TimedOut(strings::Substitute("Timed out waiting for non-empty "
                                                  "GetTableLocations reply from a leader master "
                                                  "after $0 ms.", timeout_millis));
    }
    rpc::RpcController rpc;
    rpc.set_timeout(client_->default_admin_operation_timeout());
    s = client_->data_->master_proxy_->GetTableLocations(req, &resp, &rpc);
    if (!s.ok()) {
      if (s.IsNetworkError()) {
        LOG(WARNING) << "Network error talking to the leader master ("
                     << client_->data_->leader_master_hostport().ToString() << "): "
                     << s.ToString() << ". Determining the leader master again and retrying.";
        s = client_->data_->SetMasterServerProxy(client_.get());
        if (s.ok()) {
          continue;
        }
      }
      // TODO: See KUDU-572 for more discussion on timeout handling.
      if (s.IsTimedOut()) {
        LOG(WARNING) << "Timed out talking to the leader master ("
                     << client_->data_->leader_master_hostport().ToString() << "): "
                     << s.ToString() << ". Determining the leader master again and retrying.";
        s = client_->data_->SetMasterServerProxy(client_.get());
        if (s.ok()) {
          continue;
        }
      }
    }
    if (s.ok() && resp.has_error()) {
      if (resp.error().code()  == master::MasterErrorPB::NOT_THE_LEADER ||
          resp.error().code() == master::MasterErrorPB::CATALOG_MANAGER_NOT_INITIALIZED) {
        LOG(WARNING) << "Master " << client_->data_->leader_master_hostport().ToString()
                     << " is no longer the leader master. Determining the leader master again"
            " and retrying.";
        s = client_->data_->SetMasterServerProxy(client_.get());
        if (s.ok()) {
          continue;
        }
      }
      if (s.ok()) {
        s = StatusFromPB(resp.error().status());
      }
    }
    if (!s.ok()) {
      LOG(WARNING) << "Error getting table locations: " << s.ToString() << ", retrying.";
      continue;
    }
    if (resp.tablet_locations_size() > 0) {
      break;
    }

    /* TODO: Use exponential backoff instead */
    usleep(100000);
  }

  VLOG(1) << "Open Table " << name_ << ", found " << resp.tablet_locations_size() << " tablets";
  return Status::OK();
}

} // namespace client
} // namespace kudu
