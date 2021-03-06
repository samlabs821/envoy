#include "envoy/extensions/filters/http/grpc_stats/v3/config.pb.h"
#include "envoy/extensions/filters/http/grpc_stats/v3/config.pb.validate.h"

#include "common/grpc/common.h"

#include "extensions/filters/http/grpc_stats/grpc_stats_filter.h"

#include "test/common/buffer/utility.h"
#include "test/common/stream_info/test_util.h"
#include "test/mocks/server/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::Return;

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcStats {
namespace {

class GrpcStatsFilterConfigTest : public testing::Test {
protected:
  void initialize() {
    GrpcStatsFilterConfigFactory factory;
    Http::FilterFactoryCb cb = factory.createFilterFactoryFromProto(config_, "stats", context_);
    Http::MockFilterChainFactoryCallbacks filter_callback;

    ON_CALL(filter_callback, addStreamFilter(_)).WillByDefault(testing::SaveArg<0>(&filter_));
    EXPECT_CALL(filter_callback, addStreamFilter(_));
    cb(filter_callback);

    ON_CALL(decoder_callbacks_, streamInfo()).WillByDefault(testing::ReturnRef(stream_info_));
    filter_->setDecoderFilterCallbacks(decoder_callbacks_);
  }

  void addAllowlistEntry() {
    auto* allowlist = config_.mutable_individual_method_stats_allowlist();
    auto* services = allowlist->mutable_services();
    auto* service = services->Add();
    service->set_name("BadCompanions");
    *service->mutable_method_names()->Add() = "GetBadCompanions";
    *service->mutable_method_names()->Add() = "AnotherMethod";
  }

  void doRequestResponse(Http::TestRequestHeaderMapImpl& request_headers) {
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

    Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}};
    EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));
    Buffer::OwnedImpl data("hello");
    EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(data, false));
    Http::TestResponseTrailerMapImpl response_trailers{{"grpc-status", "0"}};
    EXPECT_EQ(Http::FilterTrailersStatus::Continue, filter_->encodeTrailers(response_trailers));
  }

  envoy::extensions::filters::http::grpc_stats::v3::FilterConfig config_;
  NiceMock<Server::Configuration::MockFactoryContext> context_;
  std::shared_ptr<Http::StreamFilter> filter_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> decoder_callbacks_;
  TestStreamInfo stream_info_;
};

TEST_F(GrpcStatsFilterConfigTest, StatsHttp2HeaderOnlyResponse) {
  config_.mutable_stats_for_all_methods()->set_value(true);
  initialize();
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", "application/grpc"},
      {":path", "/lyft.users.BadCompanions/GetBadCompanions"}};

  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, true));

  Http::TestResponseHeaderMapImpl continue_headers{{":status", "100"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue,
            filter_->encode100ContinueHeaders(continue_headers));
  Http::MetadataMap metadata_map{{"metadata", "metadata"}};
  EXPECT_EQ(Http::FilterMetadataStatus::Continue, filter_->encodeMetadata(metadata_map));

  Http::TestResponseHeaderMapImpl response_headers{{":status", "200"}, {"grpc-status", "1"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, true));
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.failure")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_FALSE(stream_info_.filterState()->hasDataWithName(HttpFilterNames::get().GrpcStats));
}

TEST_F(GrpcStatsFilterConfigTest, StatsHttp2NormalResponse) {
  config_.mutable_stats_for_all_methods()->set_value(true);
  initialize();
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", "application/grpc"},
      {":path", "/lyft.users.BadCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_FALSE(stream_info_.filterState()->hasDataWithName(HttpFilterNames::get().GrpcStats));
}

TEST_F(GrpcStatsFilterConfigTest, StatsHttp2ContentTypeGrpcPlusProto) {
  config_.mutable_stats_for_all_methods()->set_value(true);
  initialize();
  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", "application/grpc+proto"},
      {":path", "/lyft.users.BadCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_FALSE(stream_info_.filterState()->hasDataWithName(HttpFilterNames::get().GrpcStats));
}

// Test that an allowlist match results in method-named stats.
TEST_F(GrpcStatsFilterConfigTest, StatsAllowlistMatch) {
  addAllowlistEntry();
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                                 {":path", "/BadCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.total")
                     .value());
}

// Test that an allowlist method mismatch results in going to the generic stat.
TEST_F(GrpcStatsFilterConfigTest, StatsAllowlistMismatchMethod) {
  addAllowlistEntry();
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                                 {":path", "/BadCompanions/GetGoodCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetGoodCompanions.success")
                     .value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetGoodCompanions.total")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.success").value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.total").value());
}

// Test that an allowlist service mismatch results in going to the generic stat.
TEST_F(GrpcStatsFilterConfigTest, StatsAllowlistMismatchService) {
  addAllowlistEntry();
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                                 {":path", "/GoodCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.GoodCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.GoodCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.success").value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.total").value());
}

// Test that any method results in going to the generic stat, when stats_for_all_methods == false.
TEST_F(GrpcStatsFilterConfigTest, DisableStatsForAllMethods) {
  config_.mutable_stats_for_all_methods()->set_value(false);
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                                 {":path", "/BadCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.success").value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.total").value());
}

// Test that any method results in a specific stat, when stats_for_all_methods isn't set
// at all.
//
// This is deprecated behavior and will be changed during the deprecation window.
TEST_F(GrpcStatsFilterConfigTest, StatsForAllMethodsDefaultSetting) {
  EXPECT_CALL(
      context_.runtime_loader_.snapshot_,
      deprecatedFeatureEnabled(
          "envoy.deprecated_features.grpc_stats_filter_enable_stats_for_all_methods_by_default", _))
      .WillOnce(Invoke([](absl::string_view, bool default_value) { return default_value; }));
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                                 {":path", "/BadCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.success").value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.total").value());
}

// Test that any method results in a specific stat, when stats_for_all_methods isn't set
// at all.
//
// This is deprecated behavior and will be changed during the deprecation window.
TEST_F(GrpcStatsFilterConfigTest, StatsForAllMethodsDefaultSettingRuntimeOverrideTrue) {
  EXPECT_CALL(
      context_.runtime_loader_.snapshot_,
      deprecatedFeatureEnabled(
          "envoy.deprecated_features.grpc_stats_filter_enable_stats_for_all_methods_by_default", _))
      .WillOnce(Return(true));
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                                 {":path", "/BadCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.success").value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.total").value());
}

// Test that the runtime override for the deprecated previous default behavior works.
TEST_F(GrpcStatsFilterConfigTest, StatsForAllMethodsDefaultSettingRuntimeOverrideFalse) {
  EXPECT_CALL(
      context_.runtime_loader_.snapshot_,
      deprecatedFeatureEnabled(
          "envoy.deprecated_features.grpc_stats_filter_enable_stats_for_all_methods_by_default", _))
      .WillOnce(Return(false));
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{{"content-type", "application/grpc"},
                                                 {":path", "/BadCompanions/GetBadCompanions"}};

  doRequestResponse(request_headers);

  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.success")
                     .value());
  EXPECT_EQ(0UL, decoder_callbacks_.clusterInfo()
                     ->statsScope()
                     .counter("grpc.BadCompanions.GetBadCompanions.total")
                     .value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.success").value());
  EXPECT_EQ(1UL, decoder_callbacks_.clusterInfo()->statsScope().counter("grpc.total").value());
}

TEST_F(GrpcStatsFilterConfigTest, MessageCounts) {
  config_.mutable_stats_for_all_methods()->set_value(true);
  config_.set_emit_filter_state(true);
  initialize();

  Http::TestRequestHeaderMapImpl request_headers{
      {"content-type", "application/grpc+proto"},
      {":path", "/lyft.users.BadCompanions/GetBadCompanions"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->decodeHeaders(request_headers, false));

  ProtobufWkt::Value v1;
  v1.set_string_value("v1");
  auto b1 = Grpc::Common::serializeToGrpcFrame(v1);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(*b1, false));
  ProtobufWkt::Value v2;
  v2.set_string_value("v2");
  auto b2 = Grpc::Common::serializeToGrpcFrame(v2);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->decodeData(*b2, true));

  EXPECT_EQ(2U, decoder_callbacks_.clusterInfo()
                    ->statsScope()
                    .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.request_message_count")
                    .value());
  EXPECT_EQ(0U,
            decoder_callbacks_.clusterInfo()
                ->statsScope()
                .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.response_message_count")
                .value());
  const auto& data = stream_info_.filterState()->getDataReadOnly<GrpcStatsObject>(
      HttpFilterNames::get().GrpcStats);
  EXPECT_EQ(2U, data.request_message_count);
  EXPECT_EQ(0U, data.response_message_count);

  Http::TestResponseHeaderMapImpl response_headers{{"content-type", "application/grpc+proto"},
                                                   {":status", "200"}};
  EXPECT_EQ(Http::FilterHeadersStatus::Continue, filter_->encodeHeaders(response_headers, false));

  Buffer::OwnedImpl buffer;
  buffer.add(*b1);
  buffer.add(*b2);
  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(buffer, false));
  EXPECT_EQ(2U, decoder_callbacks_.clusterInfo()
                    ->statsScope()
                    .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.request_message_count")
                    .value());
  EXPECT_EQ(2U,
            decoder_callbacks_.clusterInfo()
                ->statsScope()
                .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.response_message_count")
                .value());
  EXPECT_EQ(2U, data.request_message_count);
  EXPECT_EQ(2U, data.response_message_count);

  EXPECT_EQ(Http::FilterDataStatus::Continue, filter_->encodeData(*b1, true));
  EXPECT_EQ(2U, decoder_callbacks_.clusterInfo()
                    ->statsScope()
                    .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.request_message_count")
                    .value());
  EXPECT_EQ(3U,
            decoder_callbacks_.clusterInfo()
                ->statsScope()
                .counter("grpc.lyft.users.BadCompanions.GetBadCompanions.response_message_count")
                .value());
  EXPECT_EQ(2U, data.request_message_count);
  EXPECT_EQ(3U, data.response_message_count);

  auto filter_object =
      *dynamic_cast<envoy::extensions::filters::http::grpc_stats::v3::FilterObject*>(
          data.serializeAsProto().get());
  EXPECT_EQ(2U, filter_object.request_message_count());
  EXPECT_EQ(3U, filter_object.response_message_count());
}

} // namespace
} // namespace GrpcStats
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
