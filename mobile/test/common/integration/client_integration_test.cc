#include "source/common/quic/quic_transport_socket_factory.h"
#include "source/common/quic/server_codec_impl.h"
#include "source/extensions/http/header_formatters/preserve_case/preserve_case_formatter.h"
#include "source/extensions/quic/connection_id_generator/envoy_deterministic_connection_id_generator_config.h"
#include "source/extensions/quic/crypto_stream/envoy_quic_crypto_server_stream.h"
#include "source/extensions/quic/proof_source/envoy_quic_proof_source_factory_impl.h"
#include "source/extensions/transport_sockets/tls/cert_validator/default_validator.h"
#include "source/extensions/udp_packet_writer/default/config.h"

#include "test/common/integration/base_client_integration_test.h"
#include "test/common/mocks/common/mocks.h"
#include "test/integration/autonomous_upstream.h"
#include "test/test_common/test_random_generator.h"

#include "extension_registry.h"
#include "library/common/data/utility.h"
#include "library/common/main_interface.h"
#include "library/common/network/proxy_settings.h"
#include "library/common/types/c_types.h"

using testing::_;
using testing::AnyNumber;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace {

// The only thing this TestKeyValueStore does is return value_ when asked for
// initial loaded contents.
// In this case the TestKeyValueStore will be used for DNS and value will map
// www.lyft.com -> fake test upstream.
class TestKeyValueStore : public Envoy::Platform::KeyValueStore {
public:
  absl::optional<std::string> read(const std::string&) override {
    ASSERT(!value_.empty());
    return value_;
  }
  void save(std::string, std::string) override {}
  void remove(const std::string&) override {}
  void addOrUpdate(absl::string_view, absl::string_view, absl::optional<std::chrono::seconds>) {}
  absl::optional<absl::string_view> get(absl::string_view) { return {}; }
  void flush() {}
  void iterate(::Envoy::KeyValueStore::ConstIterateCb) const {}
  void setValue(std::string value) { value_ = value; }

protected:
  std::string value_;
};

class ClientIntegrationTest
    : public BaseClientIntegrationTest,
      public testing::TestWithParam<std::tuple<Network::Address::IpVersion, Http::CodecType>> {
public:
  static void SetUpTestCase() { test_key_value_store_ = std::make_shared<TestKeyValueStore>(); }
  static void TearDownTestCase() { test_key_value_store_.reset(); }

  Http::CodecType getCodecType() { return std::get<1>(GetParam()); }

  ClientIntegrationTest() : BaseClientIntegrationTest(/*ip_version=*/std::get<0>(GetParam())) {
    // For H3 tests.
    Network::forceRegisterUdpDefaultWriterFactoryFactory();
    Quic::forceRegisterEnvoyQuicCryptoServerStreamFactoryImpl();
    Quic::forceRegisterQuicHttpServerConnectionFactoryImpl();
    Quic::forceRegisterQuicServerTransportSocketConfigFactory();
    Quic::forceRegisterEnvoyQuicProofSourceFactoryImpl();
    Quic::forceRegisterEnvoyDeterministicConnectionIdGeneratorConfigFactory();
    // For H2 tests.
    Extensions::TransportSockets::Tls::forceRegisterDefaultCertValidatorFactory();
  }

  void initialize() override {
    if (getCodecType() == Http::CodecType::HTTP3) {
      setUpstreamProtocol(Http::CodecType::HTTP3);
      builder_.enablePlatformCertificatesValidation(true);
      // Create a k-v store for DNS lookup which createEnvoy() will use to point
      // www.lyft.com -> fake H3 backend.
      builder_.addKeyValueStore("reserved.platform_store", test_key_value_store_);
      builder_.enableDnsCache(true, /* save_interval_seconds */ 1);
      upstream_tls_ = true;
      add_quic_hints_ = true;
    } else if (getCodecType() == Http::CodecType::HTTP2) {
      setUpstreamProtocol(Http::CodecType::HTTP2);
      builder_.enablePlatformCertificatesValidation(true);
      upstream_tls_ = true;
    }

    BaseClientIntegrationTest::initialize();

    if (getCodecType() == Http::CodecType::HTTP3) {
      auto address = fake_upstreams_[0]->localAddress();
      auto upstream_port = fake_upstreams_[0]->localAddress()->ip()->port();
      default_request_headers_.setHost(fmt::format("www.lyft.com:{}", upstream_port));
      default_request_headers_.setScheme("https");
    } else if (getCodecType() == Http::CodecType::HTTP2) {
      default_request_headers_.setScheme("https");
    }
  }

  void SetUp() override {
    setUpstreamCount(config_helper_.bootstrap().static_resources().clusters_size());
    helper_handle_ = test::SystemHelperPeer::replaceSystemHelper();
    EXPECT_CALL(helper_handle_->mock_helper(), isCleartextPermitted(_))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(helper_handle_->mock_helper(), validateCertificateChain(_, _)).Times(AnyNumber());
    EXPECT_CALL(helper_handle_->mock_helper(), cleanupAfterCertificateValidation())
        .Times(AnyNumber());
  }

  void createEnvoy() override {
    // Allow last minute addition of QUIC hints. This is done lazily as it must be done after
    // upstreams are created.
    if (add_quic_hints_) {
      auto address = fake_upstreams_[0]->localAddress();
      auto upstream_port = fake_upstreams_[0]->localAddress()->ip()->port();
      // With canonical suffix, having a quic hint of foo.lyft.com will make
      // www.lyft.com being recognized as QUIC ready.
      builder_.addQuicCanonicalSuffix(".lyft.com");
      builder_.addQuicHint("foo.lyft.com", upstream_port);

      // Force www.lyft.com to resolve to the fake upstream. It's the only domain
      // name the certs work for so we want that in the request, but we need to
      // fake resolution to not result in a request to the real www.lyft.com
      std::string host = fmt::format("www.lyft.com:{}", upstream_port);
      std::string cache_file_value_contents =
          absl::StrCat(Network::Test::getLoopbackAddressUrlString(version_), ":",
                       fake_upstreams_[0]->localAddress()->ip()->port(), "|1000000|0");
      test_key_value_store_->setValue(absl::StrCat(host.length(), "\n", host,
                                                   cache_file_value_contents.length(), "\n",
                                                   cache_file_value_contents));
    }
    BaseClientIntegrationTest::createEnvoy();
  }

  void TearDown() override {
    if (upstream_connection_) {
      ASSERT_TRUE(upstream_connection_->close());
      ASSERT_TRUE(upstream_connection_->waitForDisconnect());
      upstream_connection_.reset();
    }
    BaseClientIntegrationTest::TearDown();
  }

  void basicTest();
  void trickleTest();
  void explicitFlowControlWithCancels(uint32_t body_size = 1000, bool terminate_engine = false);

  static std::string protocolToString(Http::CodecType type) {
    if (type == Http::CodecType::HTTP3) {
      return "Http3Upstream";
    }
    if (type == Http::CodecType::HTTP2) {
      return "Http2Upstream";
    }
    return "Http1Upstream";
  }

  static std::string testParamsToString(
      const testing::TestParamInfo<std::tuple<Network::Address::IpVersion, Http::CodecType>>
          params) {
    return fmt::format(
        "{}_{}",
        TestUtility::ipTestParamsToString(testing::TestParamInfo<Network::Address::IpVersion>(
            std::get<0>(params.param), params.index)),
        protocolToString(std::get<1>(params.param)));
  }

protected:
  std::unique_ptr<test::SystemHelperPeer::Handle> helper_handle_;
  bool add_quic_hints_ = false;
  static std::shared_ptr<TestKeyValueStore> test_key_value_store_;
  FakeHttpConnectionPtr upstream_connection_;
  FakeStreamPtr upstream_request_;
};

std::shared_ptr<TestKeyValueStore> ClientIntegrationTest::test_key_value_store_{};

INSTANTIATE_TEST_SUITE_P(
    IpVersions, ClientIntegrationTest,
    testing::Combine(testing::ValuesIn(TestEnvironment::getIpVersionsForTest()),
                     testing::ValuesIn({Http::CodecType::HTTP1, Http::CodecType::HTTP2,
                                        Http::CodecType::HTTP3})),
    ClientIntegrationTest::testParamsToString);

void ClientIntegrationTest::basicTest() {
  if (getCodecType() != Http::CodecType::HTTP1) {
    EXPECT_CALL(helper_handle_->mock_helper(), isCleartextPermitted(_)).Times(0);
    EXPECT_CALL(helper_handle_->mock_helper(), validateCertificateChain(_, _));
    EXPECT_CALL(helper_handle_->mock_helper(), cleanupAfterCertificateValidation());
  }
  Buffer::OwnedImpl request_data = Buffer::OwnedImpl("request body");
  default_request_headers_.addCopy(AutonomousStream::EXPECT_REQUEST_SIZE_BYTES,
                                   std::to_string(request_data.length()));

  stream_prototype_->setOnData([this](envoy_data c_data, bool end_stream) {
    if (end_stream) {
      EXPECT_EQ(Data::Utility::copyToString(c_data), "");
    }
    cc_.on_data_calls++;
    release_envoy_data(c_data);
  });

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), false);

  envoy_data c_data = Data::Utility::toBridgeData(request_data);
  stream_->sendData(c_data);

  Platform::RequestTrailersBuilder builder;
  std::shared_ptr<Platform::RequestTrailers> trailers =
      std::make_shared<Platform::RequestTrailers>(builder.build());
  stream_->close(trailers);

  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_GE(cc_.on_data_calls, 1);
  ASSERT_EQ(cc_.on_complete_calls, 1);
  if (upstreamProtocol() == Http::CodecType::HTTP1) {
    ASSERT_EQ(cc_.on_header_consumed_bytes_from_response, 27);
    // HTTP/1
    ASSERT_EQ(1, last_stream_final_intel_.upstream_protocol);
  } else if (upstreamProtocol() == Http::CodecType::HTTP2) {
    ASSERT_EQ(2, last_stream_final_intel_.upstream_protocol);
  } else {
    // This verifies the H3 attempt was made due to the quic hints
    absl::MutexLock l(&engine_lock_);
    std::string stats = engine_->dumpStats();
    EXPECT_TRUE((absl::StrContains(stats, "cluster.base.upstream_cx_http3_total: 1"))) << stats;
    // Make sure the client reported protocol was also HTTP/3.
    ASSERT_EQ(3, last_stream_final_intel_.upstream_protocol);
  }
}

TEST_P(ClientIntegrationTest, Basic) {
  initialize();
  basicTest();
  if (upstreamProtocol() == Http::CodecType::HTTP1) {
    ASSERT_EQ(cc_.on_complete_received_byte_count, 67);
  }
}

TEST_P(ClientIntegrationTest, LargeResponse) {
  initialize();
  std::string data(1024 * 32, 'a');
  reinterpret_cast<AutonomousUpstream*>(fake_upstreams_.front().get())->setResponseBody(data);
  basicTest();
  if (upstreamProtocol() == Http::CodecType::HTTP1) {
    ASSERT_EQ(cc_.on_complete_received_byte_count, 32828);
  } else {
    ASSERT_GE(cc_.on_complete_received_byte_count, 32000);
  }
}

void ClientIntegrationTest::trickleTest() {
  autonomous_upstream_ = false;

  initialize();

  stream_prototype_->setOnData([this](envoy_data c_data, bool) {
    if (explicit_flow_control_) {
      // Allow reading up to 100 bytes.
      stream_->readData(100);
    }
    cc_.on_data_calls++;
    release_envoy_data(c_data);
  });
  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), false);
  if (explicit_flow_control_) {
    // Allow reading up to 100 bytes
    stream_->readData(100);
  }
  Buffer::OwnedImpl request_data = Buffer::OwnedImpl("request body");
  envoy_data c_data = Data::Utility::toBridgeData(request_data);
  stream_->sendData(c_data);
  Platform::RequestTrailersBuilder builder;
  std::shared_ptr<Platform::RequestTrailers> trailers =
      std::make_shared<Platform::RequestTrailers>(builder.build());
  stream_->close(trailers);

  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*BaseIntegrationTest::dispatcher_,
                                                        upstream_connection_));
  ASSERT_TRUE(
      upstream_connection_->waitForNewStream(*BaseIntegrationTest::dispatcher_, upstream_request_));
  ASSERT_TRUE(upstream_request_->waitForEndStream(*BaseIntegrationTest::dispatcher_));

  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
  for (int i = 0; i < 10; ++i) {
    upstream_request_->encodeData(1, i == 9);
  }

  terminal_callback_.waitReady();
}

TEST_P(ClientIntegrationTest, Trickle) {
  trickleTest();
  ASSERT_LE(cc_.on_data_calls, 11);
}

TEST_P(ClientIntegrationTest, TrickleExplicitFlowControl) {
  explicit_flow_control_ = true;
  trickleTest();
  ASSERT_LE(cc_.on_data_calls, 11);
}

TEST_P(ClientIntegrationTest, ManyStreamExplicitFlowControl) {
  explicit_flow_control_ = true;
  initialize();

  default_request_headers_.addCopy(AutonomousStream::RESPONSE_SIZE_BYTES, std::to_string(1000));

  uint32_t num_requests = 100;
  std::vector<Platform::StreamPrototypeSharedPtr> prototype_streams;
  std::vector<Platform::StreamSharedPtr> streams;

  for (uint32_t i = 0; i < num_requests; ++i) {
    Platform::StreamPrototypeSharedPtr stream_prototype;
    {
      absl::MutexLock l(&engine_lock_);
      stream_prototype = engine_->streamClient()->newStreamPrototype();
    }
    Platform::StreamSharedPtr stream = (*stream_prototype).start(explicit_flow_control_);
    stream_prototype->setOnComplete(
        [this, &num_requests](envoy_stream_intel, envoy_final_stream_intel) {
          cc_.on_complete_calls++;
          if (cc_.on_complete_calls == num_requests) {
            cc_.terminal_callback->setReady();
          }
        });

    stream_prototype->setOnData([stream](envoy_data c_data, bool) {
      // Allow reading up to 10 bytes.
      stream->readData(100);
      release_envoy_data(c_data);
    });
    stream->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
    stream->readData(100);
    prototype_streams.push_back(stream_prototype);
    streams.push_back(stream);
  }
  ASSERT(streams.size() == num_requests);
  ASSERT(prototype_streams.size() == num_requests);

  terminal_callback_.waitReady();
  ASSERT_EQ(num_requests, cc_.on_complete_calls);
}

void ClientIntegrationTest::explicitFlowControlWithCancels(const uint32_t body_size,
                                                           const bool terminate_engine) {
  default_request_headers_.addCopy(AutonomousStream::RESPONSE_SIZE_BYTES,
                                   std::to_string(body_size));

  uint32_t num_requests = 100;
  std::vector<Platform::StreamPrototypeSharedPtr> prototype_streams;
  std::vector<Platform::StreamSharedPtr> streams;

  // Randomly select which request number to terminate the engine on.
  uint32_t request_for_engine_termination = 0;
  if (terminate_engine) {
    TestRandomGenerator rand;
    request_for_engine_termination = rand.random() % (num_requests / 2);
  }

  for (uint32_t i = 0; i < num_requests; ++i) {
    Platform::StreamPrototypeSharedPtr stream_prototype;
    {
      absl::MutexLock l(&engine_lock_);
      stream_prototype = engine_->streamClient()->newStreamPrototype();
    }
    Platform::StreamSharedPtr stream = (*stream_prototype).start(explicit_flow_control_);
    stream_prototype->setOnComplete(
        [this, &num_requests](envoy_stream_intel, envoy_final_stream_intel) {
          cc_.on_complete_calls++;
          if (cc_.on_complete_calls + cc_.on_cancel_calls == num_requests) {
            cc_.terminal_callback->setReady();
          }
        });
    stream_prototype->setOnCancel(
        [this, &num_requests](envoy_stream_intel, envoy_final_stream_intel) {
          cc_.on_cancel_calls++;
          if (cc_.on_complete_calls + cc_.on_cancel_calls == num_requests) {
            cc_.terminal_callback->setReady();
          }
        });
    stream_prototype->setOnData([stream](envoy_data c_data, bool) {
      // Allow reading up to 10 bytes.
      stream->readData(100);
      release_envoy_data(c_data);
    });
    stream_prototype_->setOnError(
        [](Platform::EnvoyErrorSharedPtr, envoy_stream_intel, envoy_final_stream_intel) {
          RELEASE_ASSERT(0, "unexpected");
        });

    stream->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
    prototype_streams.push_back(stream_prototype);
    streams.push_back(stream);
    if (i % 2 == 0) {
      stream->cancel();
    } else {
      stream->readData(100);
    }

    if (terminate_engine && request_for_engine_termination == i) {
      {
        absl::MutexLock l(&engine_lock_);
        ASSERT_EQ(engine_->terminate(), ENVOY_SUCCESS);
        engine_.reset();
      }
      break;
    }
  }

  if (terminate_engine) {
    // Only the cancel calls are guaranteed to have completed when engine->terminate() is called.
    EXPECT_GE(cc_.on_cancel_calls, request_for_engine_termination / 2);
  } else {
    ASSERT(streams.size() == num_requests);
    ASSERT(prototype_streams.size() == num_requests);
    terminal_callback_.waitReady();
    EXPECT_EQ(num_requests / 2, cc_.on_complete_calls);
    EXPECT_EQ(num_requests / 2, cc_.on_cancel_calls);
  }
}

TEST_P(ClientIntegrationTest, ManyStreamExplicitFlowWithCancels) {
  explicit_flow_control_ = true;
  initialize();
  explicitFlowControlWithCancels();
}

TEST_P(ClientIntegrationTest, ManyStreamExplicitFlowWithCancelsAfterComplete) {
  explicit_flow_control_ = true;
  initialize();
  explicitFlowControlWithCancels(/*body_size=*/100);
}

TEST_P(ClientIntegrationTest, ManyStreamExplicitFlowWithCancelsAfterCompleteEngineTermination) {
  explicit_flow_control_ = true;
  initialize();
  explicitFlowControlWithCancels(/*body_size=*/100, /*terminate_engine=*/true);
}

TEST_P(ClientIntegrationTest, ClearTextNotPermitted) {
  if (getCodecType() != Http::CodecType::HTTP1) {
    return;
  }
  EXPECT_CALL(helper_handle_->mock_helper(), isCleartextPermitted(_)).WillRepeatedly(Return(false));

  expect_data_streams_ = false;
  initialize();

  Buffer::OwnedImpl request_data = Buffer::OwnedImpl("request body");
  default_request_headers_.addCopy(AutonomousStream::EXPECT_REQUEST_SIZE_BYTES,
                                   std::to_string(request_data.length()));

  stream_prototype_->setOnData([this](envoy_data c_data, bool end_stream) {
    if (end_stream) {
      EXPECT_EQ(Data::Utility::copyToString(c_data), "Cleartext is not permitted");
    }
    cc_.on_data_calls++;
    release_envoy_data(c_data);
  });

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);

  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "400");
  ASSERT_EQ(cc_.on_data_calls, 1);
  ASSERT_EQ(cc_.on_complete_calls, 1);
}

TEST_P(ClientIntegrationTest, BasicHttps) {
  EXPECT_CALL(helper_handle_->mock_helper(), isCleartextPermitted(_)).Times(0);
  EXPECT_CALL(helper_handle_->mock_helper(), validateCertificateChain(_, _));
  EXPECT_CALL(helper_handle_->mock_helper(), cleanupAfterCertificateValidation());

  builder_.enablePlatformCertificatesValidation(true);

  upstream_tls_ = true;

  initialize();
  default_request_headers_.setScheme("https");

  Buffer::OwnedImpl request_data = Buffer::OwnedImpl("request body");
  default_request_headers_.addCopy(AutonomousStream::EXPECT_REQUEST_SIZE_BYTES,
                                   std::to_string(request_data.length()));

  stream_prototype_->setOnData([this](envoy_data c_data, bool end_stream) {
    if (end_stream) {
      EXPECT_EQ(Data::Utility::copyToString(c_data), "");
    } else {
      EXPECT_EQ(c_data.length, 10);
    }
    cc_.on_data_calls++;
    release_envoy_data(c_data);
  });

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), false);

  envoy_data c_data = Data::Utility::toBridgeData(request_data);
  stream_->sendData(c_data);

  Platform::RequestTrailersBuilder builder;
  std::shared_ptr<Platform::RequestTrailers> trailers =
      std::make_shared<Platform::RequestTrailers>(builder.build());
  stream_->close(trailers);

  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_GE(cc_.on_data_calls, 1);
  ASSERT_EQ(cc_.on_complete_calls, 1);
  if (upstreamProtocol() == Http::CodecType::HTTP1) {
    ASSERT_EQ(cc_.on_complete_received_byte_count, 67);
  }
}

TEST_P(ClientIntegrationTest, BasicNon2xx) {
  initialize();

  // Set response header status to be non-2xx to test that the correct stats get charged.
  reinterpret_cast<AutonomousUpstream*>(fake_upstreams_.front().get())
      ->setResponseHeaders(std::make_unique<Http::TestResponseHeaderMapImpl>(
          Http::TestResponseHeaderMapImpl({{":status", "503"}})));

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_error_calls, 0);
  ASSERT_EQ(cc_.status, "503");
  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.on_complete_calls, 1);
}

TEST_P(ClientIntegrationTest, InvalidDomain) {
  initialize();

  default_request_headers_.setHost("www.doesnotexist.com");
  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_error_calls, 1);
  ASSERT_EQ(cc_.on_headers_calls, 0);
}

TEST_P(ClientIntegrationTest, BasicBeforeResponseHeaders) {
  initialize();

  default_request_headers_.addCopy(AutonomousStream::RESET_AFTER_REQUEST, "yes");

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_error_calls, 1);
  ASSERT_EQ(cc_.on_headers_calls, 0);
}

TEST_P(ClientIntegrationTest, ResetAfterResponseHeaders) {
  autonomous_allow_incomplete_streams_ = true;
  initialize();

  default_request_headers_.addCopy(AutonomousStream::RESET_AFTER_RESPONSE_HEADERS, "yes");
  default_request_headers_.addCopy(AutonomousStream::RESPONSE_DATA_BLOCKS, "1");

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_error_calls, 1);
}

TEST_P(ClientIntegrationTest, ResetAfterHeaderOnlyResponse) {
  autonomous_allow_incomplete_streams_ = true;
  initialize();

  default_request_headers_.addCopy(AutonomousStream::RESET_AFTER_RESPONSE_HEADERS, "yes");
  default_request_headers_.addCopy(AutonomousStream::RESPONSE_DATA_BLOCKS, "0");

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), false);
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_error_calls, 1);
}

TEST_P(ClientIntegrationTest, ResetBetweenDataChunks) {
  autonomous_allow_incomplete_streams_ = true;
  initialize();

  default_request_headers_.addCopy(AutonomousStream::RESET_AFTER_RESPONSE_DATA, "yes");
  default_request_headers_.addCopy(AutonomousStream::RESPONSE_DATA_BLOCKS, "2");

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_error_calls, 1);
}

TEST_P(ClientIntegrationTest, ResetAfterData) {
  autonomous_allow_incomplete_streams_ = true;
  initialize();

  default_request_headers_.addCopy(AutonomousStream::RESET_AFTER_RESPONSE_DATA, "yes");
  default_request_headers_.addCopy(AutonomousStream::RESPONSE_DATA_BLOCKS, "1");

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_error_calls, 1);
}

TEST_P(ClientIntegrationTest, CancelBeforeRequestHeadersSent) {
  autonomous_upstream_ = false;
  initialize();
  ConditionalInitializer headers_callback;

  stream_->cancel();

  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_cancel_calls, 1);
}

TEST_P(ClientIntegrationTest, CancelAfterRequestHeadersSent) {
  initialize();

  default_request_headers_.addCopy(AutonomousStream::RESPOND_AFTER_REQUEST_HEADERS, "yes");

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), false);
  stream_->cancel();
  terminal_callback_.waitReady();
  ASSERT_EQ(cc_.on_cancel_calls, 1);
}

TEST_P(ClientIntegrationTest, CancelAfterRequestComplete) {
  autonomous_upstream_ = false;
  initialize();

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  stream_->cancel();
  terminal_callback_.waitReady();
  ASSERT_EQ(cc_.on_cancel_calls, 1);
}

TEST_P(ClientIntegrationTest, CancelDuringResponse) {
  autonomous_upstream_ = false;
  initialize();
  ConditionalInitializer headers_callback;

  stream_prototype_->setOnHeaders(
      [this, &headers_callback](Platform::ResponseHeadersSharedPtr headers, bool,
                                envoy_stream_intel) {
        cc_.status = absl::StrCat(headers->httpStatus());
        cc_.on_headers_calls++;
        headers_callback.setReady();
        return nullptr;
      });

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);

  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*BaseIntegrationTest::dispatcher_,
                                                        upstream_connection_));
  ASSERT_TRUE(
      upstream_connection_->waitForNewStream(*BaseIntegrationTest::dispatcher_, upstream_request_));
  // Send an incomplete response.
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);

  headers_callback.waitReady();
  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_data_calls, 0);
  ASSERT_EQ(cc_.on_complete_calls, 0);

  // Now cancel, and make sure the cancel is received.
  stream_->cancel();
  memset(&cc_.final_intel, 0, sizeof(cc_.final_intel));
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_data_calls, 0);
  ASSERT_EQ(cc_.on_complete_calls, 0);
  ASSERT_EQ(cc_.on_cancel_calls, 1);

  if (upstreamProtocol() != Http::CodecType::HTTP1) {
    ASSERT_TRUE(upstream_request_->waitForReset());
  }
}

TEST_P(ClientIntegrationTest, BasicCancelWithCompleteStream) {
  autonomous_upstream_ = false;

  initialize();
  ConditionalInitializer headers_callback;

  stream_prototype_->setOnHeaders(
      [this, &headers_callback](Platform::ResponseHeadersSharedPtr headers, bool,
                                envoy_stream_intel) {
        cc_.status = absl::StrCat(headers->httpStatus());
        cc_.on_headers_calls++;
        headers_callback.setReady();
        return nullptr;
      });

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);

  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*BaseIntegrationTest::dispatcher_,
                                                        upstream_connection_));
  ASSERT_TRUE(
      upstream_connection_->waitForNewStream(*BaseIntegrationTest::dispatcher_, upstream_request_));
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, true);

  terminal_callback_.waitReady();
  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_complete_calls, 1);

  // Now cancel. As on_complete has been called cancel is a no-op but is
  // non-problematic.
  stream_->cancel();
}

TEST_P(ClientIntegrationTest, CancelWithPartialStream) {
  autonomous_upstream_ = false;
  explicit_flow_control_ = true;
  initialize();
  ConditionalInitializer headers_callback;

  stream_prototype_->setOnHeaders(
      [this, &headers_callback](Platform::ResponseHeadersSharedPtr headers, bool,
                                envoy_stream_intel) {
        cc_.status = absl::StrCat(headers->httpStatus());
        cc_.on_headers_calls++;
        headers_callback.setReady();
        return nullptr;
      });

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);

  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*BaseIntegrationTest::dispatcher_,
                                                        upstream_connection_));
  ASSERT_TRUE(
      upstream_connection_->waitForNewStream(*BaseIntegrationTest::dispatcher_, upstream_request_));

  // Send a complete response with body.
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);
  upstream_request_->encodeData(1, true);

  headers_callback.waitReady();
  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_data_calls, 0);
  ASSERT_EQ(cc_.on_complete_calls, 0);

  // Due to explicit flow control, the upstream stream is complete, but the
  // callbacks will not be called for data and completion. Cancel the stream
  // and make sure the cancel is received.
  stream_->cancel();
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_data_calls, 0);
  ASSERT_EQ(cc_.on_complete_calls, 0);
  ASSERT_EQ(cc_.on_cancel_calls, 1);
}

// Test header key case sensitivity.
TEST_P(ClientIntegrationTest, CaseSensitive) {
  if (getCodecType() != Http::CodecType::HTTP1) {
    return;
  }
  autonomous_upstream_ = false;
  initialize();

  default_request_headers_.header_map_->setFormatter(
      std::make_unique<
          Extensions::Http::HeaderFormatters::PreserveCase::PreserveCaseHeaderFormatter>(
          false, envoy::extensions::http::header_formatters::preserve_case::v3::
                     PreserveCaseFormatterConfig::DEFAULT));

  default_request_headers_.addCopy("FoO", "bar");
  default_request_headers_.header_map_->formatter().value().get().processKey("FoO");

  stream_prototype_->setOnHeaders(
      [this](Platform::ResponseHeadersSharedPtr headers, bool, envoy_stream_intel) {
        cc_.status = absl::StrCat(headers->httpStatus());
        cc_.on_headers_calls++;
        EXPECT_TRUE(headers->contains("My-ResponsE-Header"));
        EXPECT_TRUE((*headers)["My-ResponsE-Header"][0] == "foo");
        return nullptr;
      });
  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);

  Envoy::FakeRawConnectionPtr upstream_connection;
  ASSERT_TRUE(fake_upstreams_[0]->waitForRawConnection(upstream_connection));

  // Verify that the upstream request has preserved cased headers.
  std::string upstream_request;
  EXPECT_TRUE(upstream_connection->waitForData(FakeRawConnection::waitForInexactMatch("GET /"),
                                               &upstream_request));
  EXPECT_TRUE(absl::StrContains(upstream_request, "FoO: bar")) << upstream_request;

  // Send mixed case headers, and verify via setOnHeaders they are received correctly.
  auto response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nMy-ResponsE-Header: foo\r\n\r\n";
  ASSERT_TRUE(upstream_connection->write(response));

  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_data_calls, 0);
  ASSERT_EQ(cc_.on_complete_calls, 1);
}

TEST_P(ClientIntegrationTest, TimeoutOnRequestPath) {
  builder_.setStreamIdleTimeoutSeconds(1);

  autonomous_upstream_ = false;
  initialize();

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), false);

  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*BaseIntegrationTest::dispatcher_,
                                                        upstream_connection_));
  ASSERT_TRUE(
      upstream_connection_->waitForNewStream(*BaseIntegrationTest::dispatcher_, upstream_request_));

  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 0);
  ASSERT_EQ(cc_.on_data_calls, 0);
  ASSERT_EQ(cc_.on_complete_calls, 0);
  ASSERT_EQ(cc_.on_error_calls, 1);

  if (getCodecType() != Http::CodecType::HTTP1) {
    ASSERT_TRUE(upstream_request_->waitForReset());
  } else {
    ASSERT_TRUE(upstream_connection_->waitForDisconnect());
  }
}

TEST_P(ClientIntegrationTest, TimeoutOnResponsePath) {
  builder_.setStreamIdleTimeoutSeconds(1);
  autonomous_upstream_ = false;
  initialize();

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);

  ASSERT_TRUE(fake_upstreams_[0]->waitForHttpConnection(*BaseIntegrationTest::dispatcher_,
                                                        upstream_connection_));
  ASSERT_TRUE(
      upstream_connection_->waitForNewStream(*BaseIntegrationTest::dispatcher_, upstream_request_));

  // Send response headers but no body.
  upstream_request_->encodeHeaders(Http::TestResponseHeaderMapImpl{{":status", "200"}}, false);

  // Wait for timeout.
  terminal_callback_.waitReady();

  ASSERT_EQ(cc_.on_headers_calls, 1);
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_data_calls, 0);
  ASSERT_EQ(cc_.on_complete_calls, 0);
  ASSERT_EQ(cc_.on_error_calls, 1);

  if (upstreamProtocol() != Http::CodecType::HTTP1) {
    ASSERT_TRUE(upstream_request_->waitForReset());
  }
}

TEST_P(ClientIntegrationTest, Proxying) {
  if (getCodecType() != Http::CodecType::HTTP1) {
    return;
  }
  builder_.addLogLevel(Platform::LogLevel::trace);
  initialize();

  set_proxy_settings(rawEngine(), fake_upstreams_[0]->localAddress()->asString().c_str(),
                     fake_upstreams_[0]->localAddress()->ip()->port());

  // The initial request will do the DNS lookup.
  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_complete_calls, 1);
  stream_.reset();

  // The second request will use the cached DNS entry and should succeed as well.
  stream_ = (*stream_prototype_).start(explicit_flow_control_);
  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();
  ASSERT_EQ(cc_.status, "200");
  ASSERT_EQ(cc_.on_complete_calls, 2);
}

TEST_P(ClientIntegrationTest, DirectResponse) {
  initialize();

  // Override to not validate stream intel.
  stream_prototype_->setOnComplete(
      [this](envoy_stream_intel, envoy_final_stream_intel final_intel) {
        cc_.on_complete_received_byte_count = final_intel.received_byte_count;
        cc_.on_complete_calls++;
        cc_.terminal_callback->setReady();
      });

  default_request_headers_.setHost("127.0.0.1");
  default_request_headers_.setPath("/");

  stream_->sendHeaders(envoyToMobileHeaders(default_request_headers_), true);
  terminal_callback_.waitReady();
  ASSERT_EQ(cc_.status, "404");
  ASSERT_EQ(cc_.on_headers_calls, 1);
  stream_.reset();

  // Verify the default runtime values.
  EXPECT_FALSE(Runtime::runtimeFeatureEnabled("envoy.reloadable_features.test_feature_false"));
  EXPECT_TRUE(Runtime::runtimeFeatureEnabled("envoy.reloadable_features.test_feature_true"));
}

TEST_P(ClientIntegrationTest, TestRuntimeSet) {
  builder_.setRuntimeGuard("test_feature_true", false);
  builder_.setRuntimeGuard("test_feature_false", true);
  initialize();

  // Verify that the Runtime config values are from the RTDS response.
  EXPECT_TRUE(Runtime::runtimeFeatureEnabled("envoy.reloadable_features.test_feature_false"));
  EXPECT_FALSE(Runtime::runtimeFeatureEnabled("envoy.reloadable_features.test_feature_true"));
}

TEST_P(ClientIntegrationTest, TestStats) {
  initialize();

  {
    absl::MutexLock l(&engine_lock_);
    std::string stats = engine_->dumpStats();
    EXPECT_TRUE((absl::StrContains(stats, "runtime.load_success: 1"))) << stats;
  }
}

} // namespace
} // namespace Envoy
