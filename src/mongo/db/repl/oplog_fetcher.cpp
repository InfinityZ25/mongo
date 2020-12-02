/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/db/repl/oplog_fetcher.h"

#include "mongo/base/counter.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/oplog_query_metadata.h"
#include "mongo/s/resharding/resume_token_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

MONGO_FAIL_POINT_DEFINE(stopReplProducer);
MONGO_FAIL_POINT_DEFINE(stopReplProducerOnDocument);
MONGO_FAIL_POINT_DEFINE(setSmallOplogGetMoreMaxTimeMS);
MONGO_FAIL_POINT_DEFINE(hangAfterOplogFetcherCallbackScheduled);
MONGO_FAIL_POINT_DEFINE(hangBeforeStartingOplogFetcher);
MONGO_FAIL_POINT_DEFINE(hangBeforeOplogFetcherRetries);
MONGO_FAIL_POINT_DEFINE(hangBeforeProcessingSuccessfulBatch);
MONGO_FAIL_POINT_DEFINE(hangOplogFetcherBeforeAdvancingLastFetched);

namespace {
class OplogBatchStats {
public:
    void recordMillis(int millis, bool isEmptyBatch);
    BSONObj getReport() const;
    operator BSONObj() const {
        return getReport();
    }

private:
    TimerStats _getMores;
    Counter64 _numEmptyBatches;
};

void OplogBatchStats::recordMillis(int millis, bool isEmptyBatch) {
    _getMores.recordMillis(millis);
    if (isEmptyBatch) {
        _numEmptyBatches.increment();
    }
}

BSONObj OplogBatchStats::getReport() const {
    BSONObjBuilder b(_getMores.getReport());
    b.append("numEmptyBatches", _numEmptyBatches.get());
    return b.obj();
}

// The number and time spent reading batches off the network
OplogBatchStats oplogBatchStats;
ServerStatusMetricField<OplogBatchStats> displayBatchesRecieved("repl.network.getmores",
                                                                &oplogBatchStats);
// The oplog entries read via the oplog reader
Counter64 opsReadStats;
ServerStatusMetricField<Counter64> displayOpsRead("repl.network.ops", &opsReadStats);
// The bytes read via the oplog reader
Counter64 networkByteStats;
ServerStatusMetricField<Counter64> displayBytesRead("repl.network.bytes", &networkByteStats);

Counter64 readersCreatedStats;
ServerStatusMetricField<Counter64> displayReadersCreated("repl.network.readersCreated",
                                                         &readersCreatedStats);

const Milliseconds maximumAwaitDataTimeoutMS(30 * 1000);

/**
 * Calculates await data timeout based on the current replica set configuration.
 */
Milliseconds calculateAwaitDataTimeout(const ReplSetConfig& config) {
    if (MONGO_unlikely(setSmallOplogGetMoreMaxTimeMS.shouldFail())) {
        return Milliseconds(50);
    }

    // Under protocol version 1, make the awaitData timeout (maxTimeMS) dependent on the election
    // timeout. This enables the sync source to communicate liveness of the primary to secondaries.
    // We never wait longer than 30 seconds.
    return std::min((config.getElectionTimeoutPeriod() / 2), maximumAwaitDataTimeoutMS);
}
}  // namespace


StatusWith<OplogFetcher::DocumentsInfo> OplogFetcher::validateDocuments(
    const OplogFetcher::Documents& documents,
    bool first,
    Timestamp lastTS,
    StartingPoint startingPoint) {
    if (first && documents.empty()) {
        return Status(ErrorCodes::OplogStartMissing,
                      str::stream() << "The first batch of oplog entries is empty, but expected at "
                                       "least 1 document matching ts: "
                                    << lastTS.toString());
    }

    DocumentsInfo info;
    // The count of the bytes of the documents read off the network.
    info.networkDocumentBytes = 0;
    info.networkDocumentCount = 0;
    for (auto&& doc : documents) {
        info.networkDocumentBytes += doc.objsize();
        ++info.networkDocumentCount;

        // If this is the first response (to the $gte query) then we already applied the first doc.
        if (first && info.networkDocumentCount == 1U) {
            continue;
        }

        auto docOpTime = OpTime::parseFromOplogEntry(doc);
        if (!docOpTime.isOK()) {
            return docOpTime.getStatus();
        }
        info.lastDocument = docOpTime.getValue();

        // Check to see if the oplog entry goes back in time for this document.
        const auto docTS = info.lastDocument.getTimestamp();
        if (lastTS >= docTS) {
            return Status(ErrorCodes::OplogOutOfOrder,
                          str::stream() << "Out of order entries in oplog. lastTS: "
                                        << lastTS.toString() << " outOfOrderTS:" << docTS.toString()
                                        << " in batch with " << info.networkDocumentCount
                                        << "docs; first-batch:" << first << ", doc:" << doc);
        }
        lastTS = docTS;
    }

    // These numbers are for the documents we will apply.
    info.toApplyDocumentCount = documents.size();
    info.toApplyDocumentBytes = info.networkDocumentBytes;
    if (first && startingPoint == StartingPoint::kSkipFirstDoc) {
        // The count is one less since the first document found was already applied ($gte $ts query)
        // and we will not apply it again.
        --info.toApplyDocumentCount;
        auto alreadyAppliedDocument = documents.cbegin();
        info.toApplyDocumentBytes -= alreadyAppliedDocument->objsize();
    }

    return info;
}

OplogFetcher::OplogFetcher(executor::TaskExecutor* executor,
                           OpTime lastFetched,
                           HostAndPort source,
                           ReplSetConfig config,
                           std::unique_ptr<OplogFetcherRestartDecision> oplogFetcherRestartDecision,
                           int requiredRBID,
                           bool requireFresherSyncSource,
                           DataReplicatorExternalState* dataReplicatorExternalState,
                           EnqueueDocumentsFn enqueueDocumentsFn,
                           OnShutdownCallbackFn onShutdownCallbackFn,
                           const int batchSize,
                           StartingPoint startingPoint,
                           BSONObj filter,
                           ReadConcernArgs readConcern,
                           bool requestResumeToken,
                           StringData name)
    : AbstractAsyncComponent(executor, name.toString()),
      _source(source),
      _requiredRBID(requiredRBID),
      _oplogFetcherRestartDecision(std::move(oplogFetcherRestartDecision)),
      _onShutdownCallbackFn(onShutdownCallbackFn),
      _lastFetched(lastFetched),
      _createClientFn(
          [] { return std::make_unique<DBClientConnection>(true /* autoReconnect */); }),
      _requireFresherSyncSource(requireFresherSyncSource),
      _dataReplicatorExternalState(dataReplicatorExternalState),
      _enqueueDocumentsFn(enqueueDocumentsFn),
      _awaitDataTimeout(calculateAwaitDataTimeout(config)),
      _batchSize(batchSize),
      _startingPoint(startingPoint),
      _queryFilter(filter),
      _queryReadConcern(readConcern),
      _requestResumeToken(requestResumeToken) {
    invariant(config.isInitialized());
    invariant(!_lastFetched.isNull());
    invariant(onShutdownCallbackFn);
    invariant(enqueueDocumentsFn);
}

OplogFetcher::~OplogFetcher() {
    shutdown();
    join();
}

void OplogFetcher::setConnection(std::unique_ptr<DBClientConnection>&& _connectedClient) {
    // Can only call this once, before startup.
    invariant(!_conn);
    _conn = std::move(_connectedClient);
}

Status OplogFetcher::_doStartup_inlock() noexcept {
    return _scheduleWorkAndSaveHandle_inlock(
        [this](const executor::TaskExecutor::CallbackArgs& args) {
            // Tests use this failpoint to prevent the oplog fetcher from starting.  If those
            // tests fail and the oplog fetcher is canceled, we want to continue so we see
            // a test failure quickly instead of a test timeout eventually.
            while (hangBeforeStartingOplogFetcher.shouldFail() && !args.myHandle.isCanceled()) {
                sleepmillis(100);
            }
            _runQuery(args);
        },
        &_runQueryHandle,
        "_runQuery");
}

void OplogFetcher::_doShutdown_inlock() noexcept {
    _cancelHandle_inlock(_runQueryHandle);

    if (_conn) {
        _conn->shutdownAndDisallowReconnect();
    }
}

Mutex* OplogFetcher::_getMutex() noexcept {
    return &_mutex;
}

std::string OplogFetcher::toString() {
    stdx::lock_guard lock(_mutex);
    str::stream output;
    output << "OplogFetcher -";
    output << " last optime fetched: " << _lastFetched.toString();
    output << " source: " << _source.toString();
    output << " namespace: " << _nss.toString();
    output << " active: " << _isActive_inlock();
    output << " shutting down?:" << _isShuttingDown_inlock();
    output << " first batch: " << _firstBatch;
    output << " initial find timeout: " << _getInitialFindMaxTime();
    output << " retried find timeout: " << _getRetriedFindMaxTime();
    output << " awaitData timeout: " << _awaitDataTimeout;
    return output;
}

OpTime OplogFetcher::getLastOpTimeFetched_forTest() const {
    return _getLastOpTimeFetched();
}

BSONObj OplogFetcher::getFindQuery_forTest(long long findTimeout) const {
    return _makeFindQuery(findTimeout);
}

Milliseconds OplogFetcher::getAwaitDataTimeout_forTest() const {
    return _awaitDataTimeout;
}

void OplogFetcher::setCreateClientFn_forTest(const CreateClientFn& createClientFn) {
    stdx::lock_guard lock(_mutex);
    _createClientFn = createClientFn;
}

DBClientConnection* OplogFetcher::getDBClientConnection_forTest() const {
    stdx::lock_guard lock(_mutex);
    return _conn.get();
}

Milliseconds OplogFetcher::getInitialFindMaxTime_forTest() const {
    return _getInitialFindMaxTime();
}

Milliseconds OplogFetcher::getRetriedFindMaxTime_forTest() const {
    return _getRetriedFindMaxTime();
}

void OplogFetcher::_setSocketTimeout(long long timeout) {
    stdx::lock_guard<Latch> lock(_mutex);
    invariant(_conn);
    // setSoTimeout takes a double representing the number of seconds for send and receive
    // timeouts. Thus, we must express the timeout in milliseconds and divide by 1000.0 to get
    // the number of seconds with a fractional part.
    _conn->setSoTimeout(timeout / 1000.0 + oplogNetworkTimeoutBufferSeconds.load());
}

OpTime OplogFetcher::_getLastOpTimeFetched() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _lastFetched;
}

Milliseconds OplogFetcher::_getInitialFindMaxTime() const {
    return Milliseconds(oplogInitialFindMaxSeconds.load() * 1000);
}

Milliseconds OplogFetcher::_getRetriedFindMaxTime() const {
    return Milliseconds(oplogRetriedFindMaxSeconds.load() * 1000);
}

void OplogFetcher::_finishCallback(Status status) {
    invariant(isActive());
    // If the oplog fetcher is shutting down, consolidate return code to CallbackCanceled.
    if (_isShuttingDown() && status != ErrorCodes::CallbackCanceled) {
        status = Status(ErrorCodes::CallbackCanceled,
                        str::stream() << "Got error: \"" << status.toString()
                                      << "\" while oplog fetcher is shutting down");
    }
    _onShutdownCallbackFn(status, _requiredRBID);

    decltype(_onShutdownCallbackFn) onShutdownCallbackFn;
    decltype(_oplogFetcherRestartDecision) oplogFetcherRestartDecision;
    stdx::lock_guard<Latch> lock(_mutex);
    _transitionToComplete_inlock();

    // Release any resources that might be held by the '_onShutdownCallbackFn' function object.
    // The function object will be destroyed outside the lock since the temporary variable
    // 'onShutdownCallbackFn' is declared before 'lock'.
    invariant(_onShutdownCallbackFn);
    std::swap(_onShutdownCallbackFn, onShutdownCallbackFn);

    // Release any resources held by the OplogFetcherRestartDecision.
    invariant(_oplogFetcherRestartDecision);
    std::swap(_oplogFetcherRestartDecision, oplogFetcherRestartDecision);
}

void OplogFetcher::_runQuery(const executor::TaskExecutor::CallbackArgs& callbackData) noexcept {
    Status responseStatus =
        _checkForShutdownAndConvertStatus(callbackData, "error running oplog fetcher");
    if (!responseStatus.isOK()) {
        _finishCallback(responseStatus);
        return;
    }

    bool hadExistingConnection = true;
    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (!_conn) {
            _conn = _createClientFn();
            hadExistingConnection = false;
        }
    }

    hangAfterOplogFetcherCallbackScheduled.pauseWhileSet();

    if (!hadExistingConnection) {
        auto connectStatus = _connect();

        // Error out if we failed to connect after exhausting the allowed retry attempts.
        if (!connectStatus.isOK()) {
            _finishCallback(connectStatus);
            return;
        }
    }

    _setMetadataWriterAndReader();
    _createNewCursor(true /* initialFind */);

    while (true) {
        Status status{Status::OK()};
        {
            // Both of these checks need to happen while holding the mutex since they could race
            // with shutdown.
            stdx::lock_guard<Latch> lock(_mutex);
            if (_isShuttingDown_inlock()) {
                status = {ErrorCodes::CallbackCanceled, "oplog fetcher shutting down"};
            } else if (_runQueryHandle.isCanceled()) {
                invariant(_getExecutor()->isShuttingDown());
                status = {ErrorCodes::CallbackCanceled, "oplog fetcher task executor shutdown"};
            }
        }
        if (!status.isOK()) {
            _finishCallback(status);
            return;
        }

        auto batchResult = _getNextBatch();
        if (!batchResult.isOK()) {
            auto brStatus = batchResult.getStatus();

            // Recreate a cursor if we have enough retries left.
            if (_oplogFetcherRestartDecision->shouldContinue(this, brStatus)) {
                hangBeforeOplogFetcherRetries.pauseWhileSet();
                _createNewCursor(false /* initialFind */);
                continue;
            } else {
                _finishCallback(brStatus);
                return;
            }
        }

        // This will advance our view of _lastFetched.
        status = _onSuccessfulBatch(batchResult.getValue());
        if (!status.isOK()) {
            // The stopReplProducer fail point expects this to return successfully. If another fail
            // point wants this to return unsuccessfully, it should use a different error code.
            if (status == ErrorCodes::FailPointEnabled) {
                _finishCallback(Status::OK());
                return;
            }

            _finishCallback(status);
            return;
        }

        if (_cursor->isDead()) {
            // This means the sync source closes the tailable cursor with a returned cursorId of 0.
            // Any users of the oplog fetcher should create a new oplog fetcher if they see a
            // successful status and would like to continue fetching more oplog entries.
            _finishCallback(Status::OK());
            return;
        }
    }
}

Status OplogFetcher::_connect() {
    Status connectStatus = Status::OK();
    do {
        if (_isShuttingDown()) {
            return Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down");
        }
        connectStatus = [&] {
            try {
                if (!connectStatus.isOK()) {
                    // If this is a retry, let the DBClientConnection handle the reconnect itself
                    // for proper backoff behavior.
                    LOGV2(23437,
                          "OplogFetcher reconnecting due to error: {error}",
                          "OplogFetcher reconnecting due to error",
                          "error"_attr = connectStatus);
                    _conn->checkConnection();
                } else {
                    uassertStatusOK(_conn->connect(_source, "OplogFetcher"));
                }
                uassertStatusOK(replAuthenticate(_conn.get())
                                    .withContext(str::stream()
                                                 << "OplogFetcher failed to authenticate to "
                                                 << _source));
                // Reset any state needed to track restarts on successful connection.
                _oplogFetcherRestartDecision->fetchSuccessful(this);
                return Status::OK();
            } catch (const DBException& e) {
                hangBeforeOplogFetcherRetries.pauseWhileSet();
                return e.toStatus();
            }
        }();
    } while (!connectStatus.isOK() &&
             _oplogFetcherRestartDecision->shouldContinue(this, connectStatus));

    return connectStatus;
}

void OplogFetcher::_setMetadataWriterAndReader() {
    invariant(_conn);

    _logicalTimeMetadataHook =
        std::make_unique<rpc::LogicalTimeMetadataHook>(getGlobalServiceContext());

    _conn->setRequestMetadataWriter([this](OperationContext* opCtx, BSONObjBuilder* metadataBob) {
        *metadataBob << rpc::kReplSetMetadataFieldName << 1;
        *metadataBob << rpc::kOplogQueryMetadataFieldName << 1;
        metadataBob->appendElements(ReadPreferenceSetting::secondaryPreferredMetadata());

        // Run LogicalTimeMetadataHook on request metadata so this matches the behavior of the
        // connections in the replication coordinator thread pool.
        return _logicalTimeMetadataHook->writeRequestMetadata(opCtx, metadataBob);
    });

    _conn->setReplyMetadataReader(
        [this](OperationContext* opCtx, const BSONObj& metadataObj, StringData source) {
            _metadataObj = metadataObj.getOwned();

            // Run LogicalTimeMetadataHook on reply metadata so this matches the behavior of the
            // connections in the replication coordinator thread pool.
            return _logicalTimeMetadataHook->readReplyMetadata(opCtx, source, _metadataObj);
        });
}

BSONObj OplogFetcher::_makeFindQuery(long long findTimeout) const {
    BSONObjBuilder queryBob;

    auto lastOpTimeFetched = _getLastOpTimeFetched();
    BSONObjBuilder filterBob(queryBob.subobjStart("query"));
    filterBob.append("ts", BSON("$gte" << lastOpTimeFetched.getTimestamp()));
    // Handle caller-provided filter.
    if (!_queryFilter.isEmpty()) {
        filterBob.append(
            "$or", BSON_ARRAY(_queryFilter << BSON("ts" << lastOpTimeFetched.getTimestamp())));
    }
    filterBob.done();

    queryBob.append("$maxTimeMS", findTimeout);
    if (_requestResumeToken) {
        queryBob.append("$hint", BSON("$natural" << 1));
        queryBob.append("$_requestResumeToken", true);
    }

    auto lastCommittedWithCurrentTerm =
        _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
    auto term = lastCommittedWithCurrentTerm.value;
    if (term != OpTime::kUninitializedTerm) {
        queryBob.append("term", term);
    }

    if (_queryReadConcern.isEmpty()) {
        // This ensures that the sync source waits for all earlier oplog writes to be visible.
        // Since Timestamp(0, 0) isn't allowed, Timestamp(0, 1) is the minimal we can use.
        queryBob.append("readConcern",
                        BSON("level"
                             << "local"
                             << "afterClusterTime" << Timestamp(0, 1)));
    } else {
        // Caller-provided read concern.
        queryBob.appendElements(_queryReadConcern.toBSON());
    }

    return queryBob.obj();
}

void OplogFetcher::_createNewCursor(bool initialFind) {
    invariant(_conn);

    // Set the socket timeout to the 'find' timeout plus a network buffer.
    auto findTimeout = durationCount<Milliseconds>(initialFind ? _getInitialFindMaxTime()
                                                               : _getRetriedFindMaxTime());
    _setSocketTimeout(findTimeout);

    _cursor =
        std::make_unique<DBClientCursor>(_conn.get(),
                                         _nss,
                                         _makeFindQuery(findTimeout),
                                         0 /* nToReturn */,
                                         0 /* nToSkip */,
                                         nullptr /* fieldsToReturn */,
                                         QueryOption_CursorTailable | QueryOption_AwaitData |
                                             (oplogFetcherUsesExhaust ? QueryOption_Exhaust : 0),
                                         _batchSize);

    _firstBatch = true;

    readersCreatedStats.increment();
}

StatusWith<OplogFetcher::Documents> OplogFetcher::_getNextBatch() {
    Documents batch;
    try {
        Timer timer;
        // If it is the first batch, we should initialize the cursor, which will run the find query.
        // Otherwise we should call more() to get the next batch.
        if (_firstBatch) {
            // Network errors manifest as exceptions that are handled in the catch block. If init
            // returns false it means that the sync source responded with nothing, which could
            // indicate a problem with the sync source.
            if (!_cursor->init()) {
                _cursor.reset();
                return {ErrorCodes::InvalidSyncSource,
                        str::stream()
                            << "Oplog fetcher could not create cursor on source: " << _source};
            }

            // This will also set maxTimeMS on the generated getMore command.
            _cursor->setAwaitDataTimeoutMS(_awaitDataTimeout);

            // The 'find' command has already been executed, so reset the socket timeout to reflect
            // the awaitData timeout with a network buffer.
            _setSocketTimeout(durationCount<Milliseconds>(_awaitDataTimeout));

            // TODO SERVER-46240: Handle batchSize 1 in DBClientCursor.
            // Due to a bug in DBClientCursor, it actually uses batchSize 2 if the given batchSize
            // is 1 for the find command. So if the given batchSize is 1, we need to set it
            // explicitly for getMores.
            if (_batchSize == 1) {
                _cursor->setBatchSize(_batchSize);
            }
        } else {
            auto lastCommittedWithCurrentTerm =
                _dataReplicatorExternalState->getCurrentTermAndLastCommittedOpTime();
            if (lastCommittedWithCurrentTerm.value != OpTime::kUninitializedTerm) {
                _cursor->setCurrentTermAndLastCommittedOpTime(lastCommittedWithCurrentTerm.value,
                                                              lastCommittedWithCurrentTerm.opTime);
            }
            _cursor->more();
        }

        while (_cursor->moreInCurrentBatch()) {
            batch.emplace_back(_cursor->nextSafe());
        }

        // This value is only used on a successful batch for metrics.repl.network.getmores. This
        // metric intentionally tracks the time taken by the initial find as well.
        _lastBatchElapsedMS = timer.millis();
    } catch (const DBException& ex) {
        if (_cursor->connectionHasPendingReplies()) {
            // Close the connection because the connection cannot be used anymore as more data is on
            // the way from the server for the exhaust stream. Thus, we have to reconnect. The
            // DBClientConnection does autoreconnect on the next network operation.
            _conn->shutdown();
        }
        return ex.toStatus("Error while getting the next batch in the oplog fetcher");
    }

    return batch;
}

Status OplogFetcher::_onSuccessfulBatch(const Documents& documents) {
    hangBeforeProcessingSuccessfulBatch.pauseWhileSet();

    if (_isShuttingDown()) {
        return Status(ErrorCodes::CallbackCanceled, "oplog fetcher shutting down");
    }

    _oplogFetcherRestartDecision->fetchSuccessful(this);

    // Stop fetching and return on fail point.
    // This fail point makes the oplog fetcher ignore the downloaded batch of operations and not
    // error out. The FailPointEnabled error will be caught by the caller.
    if (MONGO_unlikely(stopReplProducer.shouldFail())) {
        return Status(ErrorCodes::FailPointEnabled, "stopReplProducer fail point is enabled");
    }

    // Stop fetching and return when we reach a particular document. This failpoint should be used
    // with the setParameter bgSyncOplogFetcherBatchSize=1, so that documents are fetched one at a
    // time.
    {
        Status status = Status::OK();
        stopReplProducerOnDocument.executeIf(
            [&](auto&&) {
                static constexpr char message[] =
                    "stopReplProducerOnDocument fail point is enabled";
                LOGV2(21269, message);
                status = {ErrorCodes::FailPointEnabled, message};
            },
            [&](const BSONObj& data) {
                auto opCtx = cc().makeOperationContext();
                boost::intrusive_ptr<ExpressionContext> expCtx(
                    new ExpressionContext(opCtx.get(), nullptr, _nss));
                Matcher m(data["document"].Obj(), expCtx);
                // TODO SERVER-46240: Handle batchSize 1 in DBClientCursor.
                // Due to a bug in DBClientCursor, it actually uses batchSize 2 if the given
                // batchSize is 1 for the find command. So we need to check up to two documents.
                return !documents.empty() &&
                    (m.matches(documents.front()["o"].Obj()) ||
                     m.matches(documents.back()["o"].Obj()));
            });
        if (!status.isOK()) {
            return status;
        }
    }

    auto firstDocToApply = documents.cbegin();

    if (!documents.empty()) {
        LOGV2_DEBUG(21270,
                    2,
                    "oplog fetcher read {batchSize} operations from remote oplog starting at "
                    "{firstTimestamp} and ending at {lastTimestamp}",
                    "Oplog fetcher read batch from remote oplog",
                    "batchSize"_attr = documents.size(),
                    "firstTimestamp"_attr = documents.front()["ts"],
                    "lastTimestamp"_attr = documents.back()["ts"]);
    } else {
        LOGV2_DEBUG(21271, 2, "Oplog fetcher read 0 operations from remote oplog");
    }

    auto oqMetadataResult = rpc::OplogQueryMetadata::readFromMetadata(_metadataObj);
    if (!oqMetadataResult.isOK()) {
        LOGV2_ERROR(21278,
                    "invalid oplog query metadata from sync source {syncSource}: "
                    "{error}: {metadata}",
                    "Invalid oplog query metadata from sync source",
                    "syncSource"_attr = _source,
                    "error"_attr = oqMetadataResult.getStatus(),
                    "metadata"_attr = _metadataObj);
        return oqMetadataResult.getStatus();
    }
    auto oqMetadata = oqMetadataResult.getValue();

    if (_firstBatch) {
        auto status =
            _checkRemoteOplogStart(documents, oqMetadata.getLastOpApplied(), oqMetadata.getRBID());
        if (!status.isOK()) {
            // Stop oplog fetcher and execute rollback if necessary.
            return status;
        }

        LOGV2_DEBUG(21272,
                    1,
                    "oplog fetcher successfully fetched from {syncSource}",
                    "Oplog fetcher successfully fetched from sync source",
                    "syncSource"_attr = _source);

        // We do not always enqueue the first document. We elect to skip it for the following
        // reasons:
        //    1. This is the first batch and no rollback is needed. Callers specify
        //       StartingPoint::kSkipFirstDoc when they want this behavior.
        //    2. We have already enqueued that document in a previous attempt. We can get into
        //       this situation if we had a batch with StartingPoint::kEnqueueFirstDoc that failed
        //       right after that first document was enqueued. In such a scenario, we would not
        //       have advanced the lastFetched opTime, so we skip past that document to avoid
        //       duplicating it.
        //    3. We have a query filter, and the first document doesn't match that filter.  This
        //       happens on the first batch when we always accept a document with the previous
        //       fetched timestamp.

        if (_startingPoint == StartingPoint::kSkipFirstDoc) {
            firstDocToApply++;
        } else if (!_queryFilter.isEmpty()) {
            auto opCtx = cc().makeOperationContext();
            auto expCtx =
                make_intrusive<ExpressionContext>(opCtx.get(), nullptr /* collator */, _nss);
            Matcher m(_queryFilter, expCtx);
            if (!m.matches(*firstDocToApply))
                firstDocToApply++;
        }
    }

    // This lastFetched value is the last OpTime from the previous batch.
    auto previousOpTimeFetched = _getLastOpTimeFetched();

    auto validateResult = OplogFetcher::validateDocuments(
        documents, _firstBatch, previousOpTimeFetched.getTimestamp(), _startingPoint);
    if (!validateResult.isOK()) {
        return validateResult.getStatus();
    }
    auto info = validateResult.getValue();
    // If the batch is empty, set 'lastDocOpTime' to the lastFetched from the previous batch.
    auto lastDocOpTime = info.lastDocument.isNull() ? previousOpTimeFetched : info.lastDocument;

    // Process replset metadata.  It is important that this happen after we've validated the
    // first batch, so we don't progress our knowledge of the commit point from a
    // response that triggers a rollback.
    auto metadataResult = rpc::ReplSetMetadata::readFromMetadata(_metadataObj);
    if (!metadataResult.isOK()) {
        LOGV2_ERROR(21279,
                    "invalid replication metadata from sync source {syncSource}: "
                    "{error}: {metadata}",
                    "Invalid replication metadata from sync source",
                    "syncSource"_attr = _source,
                    "error"_attr = metadataResult.getStatus(),
                    "metadata"_attr = _metadataObj);
        return metadataResult.getStatus();
    }
    auto replSetMetadata = metadataResult.getValue();

    // Determine if we should stop syncing from our current sync source.
    auto changeSyncSourceAction = _dataReplicatorExternalState->shouldStopFetching(
        _source, replSetMetadata, oqMetadata, previousOpTimeFetched, lastDocOpTime);
    str::stream errMsg;
    errMsg << "sync source " << _source.toString();
    errMsg << " (config version: " << replSetMetadata.getConfigVersion();
    errMsg << "; last applied optime: " << oqMetadata.getLastOpApplied().toString();
    errMsg << "; sync source index: " << oqMetadata.getSyncSourceIndex();
    errMsg << "; has primary index: " << oqMetadata.hasPrimaryIndex();
    errMsg << ") is no longer valid";
    errMsg << " previous batch last fetched optime: " << previousOpTimeFetched.toString();
    errMsg << " current batch last fetched optime: " << lastDocOpTime.toString();

    if (changeSyncSourceAction == ChangeSyncSourceAction::kStopSyncingAndDropLastBatch) {
        return Status(ErrorCodes::InvalidSyncSource, errMsg);
    }

    _dataReplicatorExternalState->processMetadata(replSetMetadata, oqMetadata);

    // Increment stats. We read all of the docs in the query.
    opsReadStats.increment(info.networkDocumentCount);
    networkByteStats.increment(info.networkDocumentBytes);

    oplogBatchStats.recordMillis(_lastBatchElapsedMS, documents.empty());

    if (_cursor->getPostBatchResumeToken()) {
        auto pbrt = ResumeTokenOplogTimestamp::parse(
            IDLParserErrorContext("OplogFetcher PostBatchResumeToken"),
            *_cursor->getPostBatchResumeToken());
        info.resumeToken = pbrt.getTs();
    }

    auto status = _enqueueDocumentsFn(firstDocToApply, documents.cend(), info);
    if (!status.isOK()) {
        return status;
    }

    if (changeSyncSourceAction == ChangeSyncSourceAction::kStopSyncingAndEnqueueLastBatch) {
        return Status(ErrorCodes::InvalidSyncSource, errMsg);
    }

    if (MONGO_unlikely(hangOplogFetcherBeforeAdvancingLastFetched.shouldFail())) {
        hangOplogFetcherBeforeAdvancingLastFetched.pauseWhileSet();
    }

    // Start skipping the first doc after at least one doc has been enqueued in the lifetime
    // of this fetcher.
    _startingPoint = StartingPoint::kSkipFirstDoc;

    // We have now processed the batch. We should only move forward our view of _lastFetched if the
    // batch was not empty.
    if (lastDocOpTime != previousOpTimeFetched) {
        LOGV2_DEBUG(21273,
                    3,
                    "Oplog fetcher setting last fetched optime ahead after batch: {lastDocOpTime}",
                    "Oplog fetcher setting last fetched optime ahead after batch",
                    "lastDocOpTime"_attr = lastDocOpTime);

        stdx::lock_guard<Latch> lock(_mutex);
        _lastFetched = lastDocOpTime;
    }

    _firstBatch = false;
    return Status::OK();
}

Status OplogFetcher::_checkRemoteOplogStart(const OplogFetcher::Documents& documents,
                                            OpTime remoteLastOpApplied,
                                            int remoteRBID) {
    using namespace fmt::literals;

    // Once we establish our cursor, if we use rollback-via-refetch, we need to ensure that our
    // upstream node hasn't rolled back since that could cause it to not have our required minValid
    // point. The cursor will be killed if the upstream node rolls back so we don't need to keep
    // checking once the cursor is established. If we do not use rollback-via-refetch, this check is
    // not necessary, and _requiredRBID will be set to kUninitializedRollbackId in that case.
    if (_requiredRBID != ReplicationProcess::kUninitializedRollbackId &&
        remoteRBID != _requiredRBID) {
        return Status(ErrorCodes::InvalidSyncSource,
                      "Upstream node rolled back after choosing it as a sync source. Choosing "
                      "new sync source.");
    }
    // Set _requiredRBID to remoteRBID so that it can be returned when the oplog fetcher shuts down.
    _requiredRBID = remoteRBID;

    // Sometimes our remoteLastOpApplied may be stale; if we received a document with an
    // opTime later than remoteLastApplied, we can assume the remote is at least up to that
    // opTime.
    if (!documents.empty()) {
        const auto docOpTime = OpTime::parseFromOplogEntry(documents.back());
        if (docOpTime.isOK()) {
            remoteLastOpApplied = std::max(remoteLastOpApplied, docOpTime.getValue());
        }
    }

    auto lastFetched = _getLastOpTimeFetched();

    // The sync source could be behind us if it rolled back after we selected it. We could have
    // failed to detect the rollback if it occurred between sync source selection (when we check the
    // candidate is ahead of us) and sync source resolution (when we got '_requiredRBID'). If the
    // sync source is now behind us, choose a new sync source to prevent going into rollback.
    if (remoteLastOpApplied < lastFetched) {
        return Status(ErrorCodes::InvalidSyncSource,
                      "Sync source's last applied OpTime {} is older than our last fetched OpTime "
                      "{}. Choosing new sync source."_format(remoteLastOpApplied.toString(),
                                                             lastFetched.toString()));
    }

    // If '_requireFresherSyncSource' is true, we must check that the sync source's
    // lastApplied is ahead of us to prevent forming a cycle. Although we check for
    // this condition in sync source selection, if an undetected rollback occurred between sync
    // source selection and sync source resolution, this condition may no longer hold.
    // '_requireFresherSyncSource' is false for initial sync, since no other node can sync off an
    // initial syncing node, so we do not need to check for cycles. In addition, it would be
    // problematic to check this condition for initial sync, since the 'lastFetched' OpTime will
    // almost always equal the 'remoteLastApplied', since we fetch the sync source's last applied
    // OpTime to determine where to start our OplogFetcher.
    if (_requireFresherSyncSource && remoteLastOpApplied <= lastFetched) {
        return Status(ErrorCodes::InvalidSyncSource,
                      "Sync source must be ahead of me. My last fetched oplog optime: {}, latest "
                      "oplog optime of sync source: {}"_format(lastFetched.toString(),
                                                               remoteLastOpApplied.toString()));
    }

    // At this point we know that our sync source has our minValid and is not behind us, so if our
    // history diverges from our sync source's we should prefer its history and roll back ours.

    // Since we checked for rollback and our sync source is ahead of us, an empty batch means that
    // we have a higher timestamp on our last fetched OpTime than our sync source's last applied
    // OpTime, but a lower term. When this occurs, we must roll back our inconsistent oplog entry.
    if (documents.empty()) {
        return Status(ErrorCodes::OplogStartMissing, "Received an empty batch from sync source.");
    }

    const auto& o = documents.front();
    auto opTimeResult = OpTime::parseFromOplogEntry(o);

    if (!opTimeResult.isOK()) {
        return Status(ErrorCodes::InvalidBSON,
                      "our last optime fetched: {}. failed to parse optime from first oplog in "
                      "batch on source: {}: {}"_format(lastFetched.toString(),
                                                       o.toString(),
                                                       opTimeResult.getStatus().toString()));
    }
    auto opTime = opTimeResult.getValue();
    if (opTime != lastFetched) {
        Status status = _checkTooStaleToSyncFromSource(lastFetched, opTime);

        // We should never return an OK status here.
        invariant(!status.isOK());
        return status;
    }
    return Status::OK();
}

Status OplogFetcher::_checkTooStaleToSyncFromSource(const OpTime lastFetched,
                                                    const OpTime firstOpTimeInDocument) {
    // Check to see if the sync source's first oplog entry is later than 'lastFetched'. If it is, we
    // are too stale to sync from this node. If it isn't, we should go into rollback instead.
    BSONObj remoteFirstOplogEntry;
    try {
        // Query for the first oplog entry in the sync source's oplog.
        auto query = Query().sort(BSON("$natural" << 1));
        // Since this function is called after the first batch, the exhaust stream has not been
        // started yet. As a result, using the same connection is safe.
        remoteFirstOplogEntry = _conn->findOne(_nss.ns(), query);
    } catch (DBException& e) {
        // If an error occurs with the query, throw an error.
        return Status(ErrorCodes::TooStaleToSyncFromSource, e.reason());
    }

    using namespace fmt::literals;

    StatusWith<OpTime> remoteFirstOpTimeResult = OpTime::parseFromOplogEntry(remoteFirstOplogEntry);
    if (!remoteFirstOpTimeResult.isOK()) {
        return Status(
            ErrorCodes::InvalidBSON,
            "failed to parse optime from first entry in source's oplog: {}: {}"_format(
                remoteFirstOplogEntry.toString(), remoteFirstOpTimeResult.getStatus().toString()));
    }

    auto remoteFirstOpTime = remoteFirstOpTimeResult.getValue();
    if (remoteFirstOpTime.isNull()) {
        return Status(ErrorCodes::InvalidBSON,
                      "optime of first entry in source's oplog cannot be null: {}"_format(
                          remoteFirstOplogEntry.toString()));
    }

    // remoteFirstOpTime may come from a very old config, so we cannot compare their terms.
    if (lastFetched.getTimestamp() < remoteFirstOpTime.getTimestamp()) {
        // We are too stale to sync from our current sync source.
        return Status(ErrorCodes::TooStaleToSyncFromSource,
                      "we are too stale to sync from the sync source's oplog. our last fetched "
                      "timestamp is earlier than the sync source's first timestamp. our last "
                      "optime fetched: {}. sync source's first optime: {}"_format(
                          lastFetched.toString(), remoteFirstOpTime.toString()));
    }

    // If we are not too stale to sync from the source, we should go into rollback.
    std::string message =
        "the sync source's oplog and our oplog have diverged, going into rollback. our last optime "
        "fetched: {}. source's GTE: {}"_format(lastFetched.toString(),
                                               firstOpTimeInDocument.toString());
    return Status(ErrorCodes::OplogStartMissing, message);
}

bool OplogFetcher::OplogFetcherRestartDecisionDefault::shouldContinue(OplogFetcher* fetcher,
                                                                      Status status) {
    // If we try to sync from a node that is shutting down, do not attempt to reconnect.
    // We should choose a new sync source.
    if (status.code() == ErrorCodes::ShutdownInProgress) {
        LOGV2(4696202,
              "Not recreating cursor for oplog fetcher because sync source is shutting down",
              "error"_attr = redact(status));
        return false;
    }
    if (_numRestarts == _maxRestarts) {
        LOGV2(21274,
              "Error returned from oplog query (no more query restarts left): {error}",
              "Error returned from oplog query (no more query restarts left)",
              "error"_attr = redact(status));
        return false;
    }
    LOGV2(21275,
          "Recreating cursor for oplog fetcher due to error: {error}. Last fetched optime: "
          "{lastOpTimeFetched}. Attempts remaining: {attemptsRemaining}",
          "Recreating cursor for oplog fetcher due to error",
          "lastOpTimeFetched"_attr = fetcher->_getLastOpTimeFetched(),
          "attemptsRemaining"_attr = (_maxRestarts - _numRestarts),
          "error"_attr = redact(status));
    _numRestarts++;
    return true;
}

void OplogFetcher::OplogFetcherRestartDecisionDefault::fetchSuccessful(OplogFetcher* fetcher) {
    _numRestarts = 0;
}

OplogFetcher::OplogFetcherRestartDecision::~OplogFetcherRestartDecision(){};

}  // namespace repl
}  // namespace mongo
