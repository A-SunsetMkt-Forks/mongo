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

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_commands_gen.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_key_pattern_query_util.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

class MoveChunkCmd final : public TypedCommand<MoveChunkCmd> {
public:
    MoveChunkCmd()
        : TypedCommand(ClusterMoveChunkRequest::kCommandName,
                       ClusterMoveChunkRequest::kCommandAlias) {}

    using Request = ClusterMoveChunkRequest;

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return true;
    }


    std::string help() const override {
        return "Example: move chunk that contains the doc {num : 7} to shard001\n"
               "  { movechunk : 'test.foo' , find : { num : 7 } , to : 'shard0001' }\n"
               "Example: move chunk with lower bound 0 and upper bound 10 to shard001\n"
               "  { movechunk : 'test.foo' , bounds : [ { num : 0 } , { num : 10 } ] "
               " , to : 'shard001' }\n";
    }

    class Invocation : public MinimalInvocationBase {
    public:
        using MinimalInvocationBase::MinimalInvocationBase;

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::moveChunk));
        }

        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {

            Timer t;

            const auto chunkManager =
                getRefreshedCollectionRoutingInfoAssertSharded_DEPRECATED(opCtx, ns())
                    .getChunkManager();
            uassert(ErrorCodes::NamespaceNotSharded,
                    str::stream() << "Can't execute " << Request::kCommandName
                                  << " on unsharded collection " << ns().toStringForErrorMsg(),
                    chunkManager.isSharded());

            uassert(ErrorCodes::InvalidOptions,
                    "bounds can only have exactly 2 elements",
                    !request().getBounds() || request().getBounds()->size() == 2);

            uassert(ErrorCodes::InvalidOptions,
                    "cannot specify bounds and query at the same time",
                    !(request().getFind() && request().getBounds()));

            uassert(ErrorCodes::InvalidOptions,
                    "need to specify query or bounds",
                    request().getFind() || request().getBounds());


            std::string destination = std::string{request().getTo()};
            const auto toStatus = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, destination);

            if (!toStatus.isOK()) {
                LOGV2_OPTIONS(22755,
                              {logv2::UserAssertAfterLog(ErrorCodes::ShardNotFound)},
                              "moveChunk destination shard does not exist",
                              "toShardId"_attr = destination,
                              logAttrs(ns()));
            }


            const auto to = toStatus.getValue();

            auto find = request().getFind();
            auto bounds = request().getBounds();


            boost::optional<Chunk> chunk;

            if (find) {
                // find
                BSONObj shardKey = uassertStatusOK(extractShardKeyFromBasicQuery(
                    opCtx, ns(), chunkManager.getShardKeyPattern(), *find));

                uassert(656450,
                        str::stream() << "no shard key found in chunk query " << *find,
                        !shardKey.isEmpty());

                if (find && chunkManager.getShardKeyPattern().isHashedPattern()) {
                    LOGV2_WARNING(7065400,
                                  "bounds should be used instead of query for hashed shard keys");
                }

                chunk.emplace(chunkManager.findIntersectingChunkWithSimpleCollation(shardKey));
            } else {

                auto minBound = bounds->front();
                auto maxBound = bounds->back();
                uassert(656451,
                        str::stream() << "shard key bounds "
                                      << "[" << minBound << "," << maxBound << ")"
                                      << " are not valid for shard key pattern "
                                      << chunkManager.getShardKeyPattern().toBSON(),
                        chunkManager.getShardKeyPattern().isShardKey(minBound) &&
                            chunkManager.getShardKeyPattern().isShardKey(maxBound));

                BSONObj minKey = chunkManager.getShardKeyPattern().normalizeShardKey(minBound);
                BSONObj maxKey = chunkManager.getShardKeyPattern().normalizeShardKey(maxBound);

                chunk.emplace(chunkManager.findIntersectingChunkWithSimpleCollation(minKey));
                uassert(656452,
                        str::stream() << "no chunk found with the shard key bounds "
                                      << "[" << minKey << "," << maxKey << ")",
                        chunk->getMin().woCompare(minKey) == 0 &&
                            chunk->getMax().woCompare(maxKey) == 0);
            }


            MoveRangeRequestBase moveRangeReq;
            moveRangeReq.setToShard(to->getId());
            moveRangeReq.setMin(chunk->getMin());
            moveRangeReq.setMax(chunk->getMax());
            moveRangeReq.setWaitForDelete(request().getWaitForDelete().value_or(false) ||
                                          request().get_waitForDelete().value_or(false));


            ConfigsvrMoveRange configsvrRequest(ns());
            configsvrRequest.setDbName(DatabaseName::kAdmin);
            configsvrRequest.setMoveRangeRequestBase(moveRangeReq);

            const auto secondaryThrottle = uassertStatusOK(
                MigrationSecondaryThrottleOptions::createFromCommand(request().toBSON()));

            configsvrRequest.setSecondaryThrottle(secondaryThrottle);

            configsvrRequest.setForceJumbo(request().getForceJumbo() ? ForceJumbo::kForceManual
                                                                     : ForceJumbo::kDoNotForce);
            generic_argument_util::setMajorityWriteConcern(configsvrRequest);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto commandResponse =
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        DatabaseName::kAdmin,
                                        configsvrRequest.toBSON(),
                                        Shard::RetryPolicy::kIdempotent);
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(commandResponse)));

            Grid::get(opCtx)->catalogCache()->onStaleCollectionVersion(ns(), boost::none);

            BSONObjBuilder resultbson;
            resultbson.append("millis", t.millis());
            result->getBodyBuilder().appendElements(resultbson.obj());
        }
    };
};
MONGO_REGISTER_COMMAND(MoveChunkCmd).forRouter();

}  // namespace
}  // namespace mongo
