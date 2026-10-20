#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_container_properties.hpp>
#include <catch2/matchers/catch_matchers_contains.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

using Catch::Matchers::Equals;

#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <future>
#include <gst-plugin/audio.hpp>
#include <gst-plugin/video.hpp>
#include <gst/app/gstappsrc.h>
#include <moonlight/fec.hpp>
#include <streaming/streaming.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace std::string_literals;

/* UTILS */

static std::pair<char * /* data */, unsigned long /* size */> copy_buffer_data(GstBuffer *buf) {
  auto size = gst_buffer_get_size(buf);
  auto *res = new char[size];

  GstMapInfo info;
  gst_buffer_map(buf, &info, GST_MAP_READ);
  std::copy(info.data, info.data + size, res);
  gst_buffer_unmap(buf, &info);

  return {res, size};
}

static audio::AudioRTPHeaders *get_rtp_audio_from_buf(GstBuffer *buf) {
  return (audio::AudioRTPHeaders *)copy_buffer_data(buf).first;
}

static std::string get_str_from_buf(GstBuffer *buf) {
  auto copy = copy_buffer_data(buf);
  return {copy.first, copy.second};
}

static int get_buf_refcount(GstBuffer *buf) {
  return buf->mini_object.refcount;
}

class GStreamerTestsFixture {
public:
  GStreamerTestsFixture() {
    gst_init(nullptr, nullptr);
    moonlight::fec::init();
  }
};

/*
 * BASE UTILS
 */

TEST_CASE_METHOD(GStreamerTestsFixture, "Basic utils", "[GSTPlugin]") {
  auto buffer = gst_buffer_new_and_fill(10, 0);
  REQUIRE_THAT(gst_buffer_copy_content(buffer),
               Catch::Matchers::SizeIs(10) && Equals(std::vector<unsigned char>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  /* Cleanup */
  REQUIRE(get_buf_refcount(buffer) == 1);
  gst_buffer_unref(buffer);

  auto payload = "char array"s;
  buffer = gst_buffer_new_and_fill(10, payload.c_str());
  REQUIRE_THAT(
      gst_buffer_copy_content(buffer),
      Catch::Matchers::SizeIs(payload.size()) && Equals(std::vector<unsigned char>(payload.begin(), payload.end())));
  REQUIRE_THAT(get_str_from_buf(buffer), Catch::Matchers::SizeIs(payload.size()) && Equals(payload));

  /* Cleanup */
  REQUIRE(get_buf_refcount(buffer) == 1);
  gst_buffer_unref(buffer);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Encrypt GstBuffer", "[GSTPlugin]") {
  auto payload = gst_buffer_new_and_fill(10, "$A PAYLOAD");
  auto aes_key = "0123456789012345"s;
  auto aes_iv = "12345678"s;
  auto cur_seq_number = 0;

  auto iv_str = derive_iv(aes_iv, cur_seq_number);

  REQUIRE_THAT(iv_str, Equals("\000\274aN\000\000\000\000\000\000\000\000\000\000\000\000"s));
  // Negative rikeyids (Moonlight sends a signed int) wrap like std::stoul did; garbage must not throw
  REQUIRE_THAT(derive_iv("-1", 0), Equals("\377\377\377\377\000\000\000\000\000\000\000\000\000\000\000\000"s));
  REQUIRE_NOTHROW(derive_iv("abc", 0));

  auto encrypted = encrypt_payload(aes_key, iv_str, payload);
  auto encrypted_str = get_str_from_buf(encrypted);

  auto decrypted = crypto::aes_decrypt_cbc(encrypted_str, aes_key, iv_str, true);
  REQUIRE_THAT(gst_buffer_copy_content(payload),
               Equals(std::vector<unsigned char>(decrypted.begin(), decrypted.end())));

  /* Cleanup */
  REQUIRE(get_buf_refcount(payload) == 1);
  gst_buffer_unref(payload);
}

/*
 * VIDEO
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "RTP VIDEO Splits", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);

  auto payload_str = "Never gonna give you up\n"
                     "Never gonna let you down\n"
                     "Never gonna run around and desert you\n"
                     "Never gonna make you cry\n"
                     "Never gonna say goodbye\n"
                     "Never gonna tell a lie and hurt you"s;

  auto rtp_header_size = (int)sizeof(gst_moonlight_video::VideoRTPHeaders);
  auto rtp_payload_header_size = 8; // 017charss
  rtpmoonlightpay->payload_size = 32;
  rtpmoonlightpay->fec_percentage = 50;
  rtpmoonlightpay->add_padding = false;

  auto payload_buf = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
  auto rtp_packets = gst_moonlight_video::split_into_rtp(rtpmoonlightpay, payload_buf);

  auto payload_expected_packets = std::ceil((float)(payload_str.size() + rtp_payload_header_size) /
                                            ((float)rtpmoonlightpay->payload_size - MAX_RTP_HEADER_SIZE));
  auto fec_expected_packets = std::ceil(payload_expected_packets * ((double)rtpmoonlightpay->fec_percentage / 100));

  REQUIRE(gst_buffer_list_length(rtp_packets) == payload_expected_packets + fec_expected_packets);

  std::string returned_payload = ""s;
  for (auto i = 0; i < payload_expected_packets; i++) {
    auto buf = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, i), rtp_header_size);
    returned_payload +=
        std::string(buf.begin() + (long)(i == 0 ? sizeof(gst_moonlight_video::VideoShortHeader) : 0), buf.end());
  }
  REQUIRE_THAT(returned_payload, Equals(payload_str));

  SECTION("Multi block FEC") {
    auto payload_buf_blocks = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
    auto rtp_packets_blocks = gst_moonlight_video::generate_rtp_packets(*rtpmoonlightpay, payload_buf_blocks);
    auto final_packets = gst_moonlight_video::generate_fec_multi_blocks(rtpmoonlightpay,
                                                                        rtp_packets_blocks,
                                                                        (int)payload_expected_packets,
                                                                        payload_buf_blocks);

    REQUIRE(gst_buffer_list_length(final_packets) ==
            payload_expected_packets + fec_expected_packets - 1); // TODO: why one less?

    auto first_payload = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 0), rtp_header_size);
    REQUIRE_THAT(
        std::string(first_payload.begin() + sizeof(gst_moonlight_video::VideoShortHeader), first_payload.end()),
        Equals("Never go"));
    // TODO: proper check content and FEC
  }

  /* Cleanup */
  REQUIRE(GST_OBJECT_REFCOUNT(rtpmoonlightpay) == 1);
  g_object_unref(rtpmoonlightpay);
  REQUIRE(get_buf_refcount(payload_buf) == 1);
  gst_buffer_unref(payload_buf);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Create RTP VIDEO packets", "[GSTPlugin]") {
  auto rtpmoonlightpay = (gst_rtp_moonlight_pay_video *)g_object_new(gst_TYPE_rtp_moonlight_pay_video, nullptr);

  rtpmoonlightpay->payload_size = 10 + MAX_RTP_HEADER_SIZE; // This will include 8bytes of payload header (017charss)
  rtpmoonlightpay->fec_percentage = 50;
  rtpmoonlightpay->add_padding = true;
  auto rtp_packet_size = rtpmoonlightpay->payload_size + sizeof(moonlight::NV_VIDEO_PACKET);
  auto rtp_header_size = (long)sizeof(gst_moonlight_video::VideoRTPHeaders);

  auto payload = gst_buffer_new_and_fill(10, "$A PAYLOAD");
  auto video_payload = gst_moonlight_video::prepend_video_header(*rtpmoonlightpay, payload);
  auto rtp_packets = gst_moonlight_video::generate_rtp_packets(*rtpmoonlightpay, video_payload);

  // 10 bytes of actual payload + 8 bytes of payload header
  // will be splitted in two RTP packets
  REQUIRE(gst_buffer_get_size(video_payload) == gst_buffer_get_size(payload) + 8); // Added 017charss
  REQUIRE(gst_buffer_list_length(rtp_packets) == 2);

  SECTION("First packet") {
    auto first_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 0));
    auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(first_packet.data());

    REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_SOF);
    REQUIRE(rtp_packet->packet.frameIndex == 0);
    REQUIRE(rtp_packet->packet.streamPacketIndex == 0);
    REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)0));

    auto short_header =
        reinterpret_cast<gst_moonlight_video::VideoShortHeader *>(first_packet.data() + rtp_header_size);
    REQUIRE(short_header->frame_type == 2);
    REQUIRE(short_header->header_type == 1);
    REQUIRE(short_header->last_payload_len == 8);

    auto rtp_payload = std::string(
        first_packet.begin() + rtp_header_size + sizeof(gst_moonlight_video::VideoShortHeader),
        first_packet.end());
    REQUIRE_THAT("$A"s, Equals(rtp_payload));
  }

  SECTION("Second packet") {
    auto second_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 1));
    auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(second_packet.data());

    REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_EOF);
    REQUIRE(rtp_packet->packet.frameIndex == 0);
    REQUIRE(rtp_packet->packet.streamPacketIndex == 0x100);
    REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)1));

    auto rtp_payload = std::string(second_packet.begin() + rtp_header_size, second_packet.end());
    REQUIRE_THAT(" PAYLOAD\0\0"s, Equals(rtp_payload));
  }

  SECTION("FEC") {
    gst_moonlight_video::generate_fec_packets(*rtpmoonlightpay, rtp_packets, payload);
    // Will append min_required_fec_packets to the original payload packets
    REQUIRE(gst_buffer_list_length(rtp_packets) == 4);

    SECTION("First packet (payload)") {
      auto first_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 0));

      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(first_packet.data());

      REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_SOF);
      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->packet.streamPacketIndex == 0);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)0));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8390208);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);

      auto short_header =
          reinterpret_cast<gst_moonlight_video::VideoShortHeader *>(first_packet.data() + rtp_header_size);
      REQUIRE(short_header->frame_type == 2);
      REQUIRE(short_header->header_type == 1);
      REQUIRE(short_header->last_payload_len == 8);

      auto rtp_payload = std::string(
          first_packet.begin() + rtp_header_size + sizeof(gst_moonlight_video::VideoShortHeader),
          first_packet.end());
      REQUIRE_THAT("$A"s, Equals(rtp_payload));
    }

    SECTION("Second packet (payload)") {
      auto second_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 1));
      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(second_packet.data());

      REQUIRE(rtp_packet->packet.flags == FLAG_CONTAINS_PIC_DATA + FLAG_EOF);
      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->packet.streamPacketIndex == 0x100);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)1));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8394304);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);

      auto rtp_payload = std::string(second_packet.begin() + rtp_header_size, second_packet.end());
      REQUIRE_THAT(" PAYLOAD\0\0"s, Equals(rtp_payload));
    }

    SECTION("Third packet (FEC)") {
      auto third_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 2));
      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(third_packet.data());

      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)2));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8398400);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);
    }

    SECTION("Fourth packet (FEC)") {
      auto fourth_packet = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 3));
      auto rtp_packet = reinterpret_cast<gst_moonlight_video::VideoRTPHeaders *>(fourth_packet.data());

      REQUIRE(rtp_packet->packet.frameIndex == 0);
      REQUIRE(rtp_packet->rtp.sequenceNumber == boost::endian::native_to_big((uint16_t)3));

      // FEC additional info
      REQUIRE(rtp_packet->packet.fecInfo == 8402496);
      REQUIRE(rtp_packet->packet.multiFecBlocks == 0);
      REQUIRE(rtp_packet->packet.multiFecFlags == 0x10);
    }

    SECTION("REED SOLOMON") {
      auto data_shards = 2;
      auto parity_shards = 2;
      auto total_shards = data_shards + parity_shards;

      auto flatten_packets = gst_buffer_list_unfold(rtp_packets);
      auto packets_content = gst_buffer_copy_content(flatten_packets);

      std::vector<unsigned char *> packets_ptr(total_shards);
      for (int shard_idx = 0; shard_idx < total_shards; shard_idx++) {
        packets_ptr[shard_idx] = &packets_content.front() + (shard_idx * rtp_packet_size);
      }

      SECTION("If no package is marked nothing should change") {
        std::vector<unsigned char> marks = {0, 0, 0, 0};

        auto rs = moonlight::fec::create(data_shards, parity_shards);
        auto result =
            moonlight::fec::decode(rs.get(), &packets_ptr.front(), &marks.front(), total_shards, rtp_packet_size);

        REQUIRE(result == 0);
        REQUIRE_THAT(packets_content, Equals(gst_buffer_copy_content(flatten_packets)));
      }

      SECTION("Missing one packet should still lead to successfully reconstruct") {
        auto missing_pkt = std::vector<unsigned char>(rtp_packet_size);
        packets_ptr[0] = &missing_pkt[0];
        std::vector<unsigned char> marks = {1, 0, 0, 0};

        auto rs = moonlight::fec::create(data_shards, parity_shards);
        auto result =
            moonlight::fec::decode(rs.get(), &packets_ptr.front(), &marks.front(), total_shards, rtp_packet_size);

        REQUIRE(result == 0);

        // Here the packet headers will be wrongly reconstructed because we are manually
        // modifying the parity packets after creation
        // We can only check the packet payload here which should be correctly reconstructed
        auto pay_size = rtpmoonlightpay->payload_size - MAX_RTP_HEADER_SIZE;
        auto missing_pkt_payload = std::vector<unsigned char>(
            missing_pkt.begin() + sizeof(gst_moonlight_video::VideoRTPHeaders),
            missing_pkt.begin() + sizeof(gst_moonlight_video::VideoRTPHeaders) + pay_size);
        auto first_packet_pay_before_fec = gst_buffer_copy_content(gst_buffer_list_get(rtp_packets, 0),
                                                                   sizeof(gst_moonlight_video::VideoRTPHeaders),
                                                                   pay_size);

        REQUIRE_THAT(missing_pkt_payload, Equals(first_packet_pay_before_fec));
      }
    }
  }

  REQUIRE(GST_OBJECT_REFCOUNT(rtpmoonlightpay) == 1);
  g_object_unref(rtpmoonlightpay);
  REQUIRE(get_buf_refcount(payload) == 1);
  gst_buffer_unref(payload);
  gst_buffer_list_unref(rtp_packets);
  REQUIRE(get_buf_refcount(video_payload) == 1);
  gst_buffer_unref(video_payload);
}

/*
 * AUDIO
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "Audio RTP packet creation", "[GSTPlugin]") {
  auto rtpmoonlightpay = std::shared_ptr<gst_rtp_moonlight_pay_audio>(
      (gst_rtp_moonlight_pay_audio *)g_object_new(gst_TYPE_rtp_moonlight_pay_audio, nullptr),
      g_object_unref);

  rtpmoonlightpay->encrypt = true;
  rtpmoonlightpay->aes_key = "0123456789012345";
  rtpmoonlightpay->aes_iv = "12345678";

  auto payload_str = "TUNZ TUNZ TUMP TUMP!"s;
  auto payload = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
  auto rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);

  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 1);
  auto first_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("First packet") {
    auto rtp_packet = get_rtp_audio_from_buf(first_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(rtp_packet->rtp.sequenceNumber == 0);
    REQUIRE(rtp_packet->rtp.timestamp == 0);

    auto rtp_payload = gst_buffer_copy_content(first_pkt, sizeof(audio::AudioRTPHeaders));

    auto decrypted = crypto::aes_decrypt_cbc(std::string(rtp_payload.begin(), rtp_payload.end()),
                                             rtpmoonlightpay->aes_key,
                                             derive_iv(rtpmoonlightpay->aes_iv, rtpmoonlightpay->cur_seq_number - 1),
                                             true);
    REQUIRE_THAT(decrypted, Equals(payload_str));
  }

  rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);
  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 2);
  auto second_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("Second packet") {
    auto rtp_packet = get_rtp_audio_from_buf(second_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.sequenceNumber) == 1);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.timestamp) == 5);

    auto rtp_payload = gst_buffer_copy_content(second_pkt, sizeof(audio::AudioRTPHeaders));

    auto decrypted = crypto::aes_decrypt_cbc(std::string(rtp_payload.begin(), rtp_payload.end()),
                                             rtpmoonlightpay->aes_key,
                                             derive_iv(rtpmoonlightpay->aes_iv, rtpmoonlightpay->cur_seq_number - 1),
                                             true);
    REQUIRE_THAT(decrypted, Equals(payload_str));
  }

  rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);
  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 3);
  auto third_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("Third packet") {
    auto rtp_packet = get_rtp_audio_from_buf(third_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.sequenceNumber) == 2);
    REQUIRE(boost::endian::big_to_native(rtp_packet->rtp.timestamp) == 10);

    auto rtp_payload = gst_buffer_copy_content(third_pkt, sizeof(audio::AudioRTPHeaders));

    auto decrypted = crypto::aes_decrypt_cbc(std::string(rtp_payload.begin(), rtp_payload.end()),
                                             rtpmoonlightpay->aes_key,
                                             derive_iv(rtpmoonlightpay->aes_iv, rtpmoonlightpay->cur_seq_number - 1),
                                             true);
    REQUIRE_THAT(decrypted, Equals(payload_str));
  }

  /* When the 4th packet arrives, we'll also FEC encode all the previous and return
   * the data packet + 2 more FEC packets
   */
  rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);
  REQUIRE(gst_buffer_list_length(rtp_packets) == 3); // One data packet + 2 FEC packets
  REQUIRE(rtpmoonlightpay->cur_seq_number == 4);

  SECTION("FEC") {
    SECTION("First FEC packet") {
      auto fec_packet = (audio::AudioFECPacket *)copy_buffer_data((gst_buffer_list_get(rtp_packets, 1))).first;

      REQUIRE(fec_packet->rtp.ssrc == 0);
      REQUIRE(fec_packet->rtp.packetType == 127);
      REQUIRE(fec_packet->rtp.header == 0x80);
      REQUIRE(fec_packet->rtp.timestamp == 0);

      REQUIRE(boost::endian::big_to_native(fec_packet->rtp.sequenceNumber) == 3);
      REQUIRE(fec_packet->fec_header.payloadType == 97);
      REQUIRE(fec_packet->fec_header.ssrc == 0);
      REQUIRE(fec_packet->fec_header.fecShardIndex == 0);
    }

    SECTION("Second FEC packet") {
      auto fec_packet = (audio::AudioFECPacket *)copy_buffer_data((gst_buffer_list_get(rtp_packets, 2))).first;

      REQUIRE(fec_packet->rtp.ssrc == 0);
      REQUIRE(fec_packet->rtp.packetType == 127);
      REQUIRE(fec_packet->rtp.header == 0x80);
      REQUIRE(fec_packet->rtp.timestamp == 0);

      REQUIRE(boost::endian::big_to_native(fec_packet->rtp.sequenceNumber) == 4);
      REQUIRE(fec_packet->fec_header.payloadType == 97);
      REQUIRE(fec_packet->fec_header.ssrc == 0);
      REQUIRE(fec_packet->fec_header.fecShardIndex == 1);
    }
  }

  SECTION("REED SOLOMON") {
    auto packet_size = gst_buffer_get_size(gst_buffer_list_get(rtp_packets, 0));

    SECTION("If no package is marked nothing should change") {
      std::vector<unsigned char> marks = {0, 0, 0, 0, 0, 0};

      auto result = moonlight::fec::decode(rtpmoonlightpay->rs.get(),
                                           rtpmoonlightpay->packets_buffer,
                                           &marks.front(),
                                           AUDIO_TOTAL_SHARDS,
                                           packet_size);

      REQUIRE(result == 0);
    }

    SECTION("Missing one packet should still lead to successful reconstruct") {
      auto original_pkt = gst_buffer_copy_content(first_pkt, sizeof(audio::AudioRTPHeaders));
      auto missing_pkt = std::vector<unsigned char>(packet_size);
      std::copy(missing_pkt.begin(), missing_pkt.end(), rtpmoonlightpay->packets_buffer[0]);
      std::vector<unsigned char> marks = {1, 0, 0, 0, 0, 0};

      auto result = moonlight::fec::decode(rtpmoonlightpay->rs.get(),
                                           rtpmoonlightpay->packets_buffer,
                                           &marks.front(),
                                           AUDIO_TOTAL_SHARDS,
                                           packet_size);

      REQUIRE(result == 0);
      REQUIRE_THAT(std::string(rtpmoonlightpay->packets_buffer[0] + sizeof(audio::AudioRTPHeaders),
                               rtpmoonlightpay->packets_buffer[0] + packet_size),
                   Equals(std::string(original_pkt.begin(), original_pkt.end())));
    }
  }
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Audio payloader refuses a missing aes_key", "[GSTPlugin]") {
  // An empty aes_key= in the pipeline string arrives as a NULL string: it used to be dereferenced (hex_to_str(NULL))
  auto pay = (gst_rtp_moonlight_pay_audio *)g_object_new(gst_TYPE_rtp_moonlight_pay_audio, nullptr);
  g_object_set(pay, "aes_key", nullptr, "aes_iv", nullptr, nullptr);
  REQUIRE(pay->aes_key.empty());
  REQUIRE(pay->aes_iv.empty());
  g_object_unref(pay);

  // Encrypting with it would read 16 bytes of key: the stream errors out instead
  gst_element_register(nullptr, "rtpmoonlightpay_audio", GST_RANK_PRIMARY, gst_TYPE_rtp_moonlight_pay_audio);
  GError *error = nullptr;
  auto pipeline = gst_parse_launch("appsrc name=src ! rtpmoonlightpay_audio name=pay encrypt=true ! fakesink", &error);
  REQUIRE(error == nullptr);
  auto pay_el = gst_bin_get_by_name(GST_BIN(pipeline), "pay");
  g_object_set(pay_el, "aes_key", nullptr, nullptr);
  gst_object_unref(pay_el);
  auto src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  auto payload_str = "TUNZ TUNZ TUMP TUMP!"s;
  gst_app_src_push_buffer(GST_APP_SRC(src), gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str()));
  auto bus = gst_element_get_bus(pipeline);
  auto msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
  REQUIRE(msg != nullptr);
  gst_message_unref(msg);
  gst_object_unref(bus);
  gst_object_unref(src);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Audio RTP without encryption", "[GSTPlugin]") {
  auto rtpmoonlightpay = std::shared_ptr<gst_rtp_moonlight_pay_audio>(
      (gst_rtp_moonlight_pay_audio *)g_object_new(gst_TYPE_rtp_moonlight_pay_audio, nullptr),
      g_object_unref);

  rtpmoonlightpay->encrypt = false;

  auto payload_str = "TUNZ TUNZ TUMP TUMP!"s;
  auto payload = gst_buffer_new_and_fill(payload_str.size(), payload_str.c_str());
  auto rtp_packets = audio::split_into_rtp(rtpmoonlightpay.get(), payload);

  REQUIRE(gst_buffer_list_length(rtp_packets) == 1);
  REQUIRE(rtpmoonlightpay->cur_seq_number == 1);
  auto first_pkt = gst_buffer_list_get(rtp_packets, 0);

  SECTION("First packet") {
    auto rtp_packet = get_rtp_audio_from_buf(first_pkt);

    REQUIRE(rtp_packet->rtp.ssrc == 0);
    REQUIRE(rtp_packet->rtp.packetType == 97);
    REQUIRE(rtp_packet->rtp.header == 0x80);
    REQUIRE(rtp_packet->rtp.sequenceNumber == 0);
    REQUIRE(rtp_packet->rtp.timestamp == 0);

    auto rtp_payload = gst_buffer_copy_content(first_pkt, sizeof(audio::AudioRTPHeaders));
    REQUIRE_THAT(std::string(rtp_payload.begin(), rtp_payload.end()), Equals(payload_str));
  }
}

/*
 * A stuck pipeline must not hold the event bus.
 * The bus calls handlers under its lock; if a stop/pause handler blocked on the stuck streaming thread,
 * every later register/unregister (e.g. starting a new runner) would hang the whole server.
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "Stopping a stuck pipeline doesn't block the event bus", "[GSTPlugin]") {
  using namespace wolf::core;
  using namespace std::chrono_literals;

  // The first buffer passes (so the pipeline is PLAYING), then each buffer parks the streaming thread for 3s
  auto video_session = [](std::size_t id) {
    return immer::box<events::VideoSession>(events::VideoSession{
        .display_mode = {.width = 64, .height = 64, .refreshRate = 30},
        .gst_pipeline = "videotestsrc is-live=true ! identity sleep-time=3000000 ! fakesink sync=false",
        .session_id = id,
        .timeout_ms = 1000,
        .packet_size = 1024,
        .fec_percentage = 20,
        .min_required_fec_packets = 2,
        .bitrate_kbps = 1000,
        .slices_per_frame = 1,
        .client_ip = "127.0.0.1"});
  };
  auto audio_session = [](std::size_t id) {
    return immer::box<events::AudioSession>(events::AudioSession{
        .gst_pipeline = "audiotestsrc is-live=true ! identity sleep-time=3000000 ! fakesink sync=false",
        .session_id = id,
        .client_ip = "127.0.0.1",
        .packet_duration = 5,
        .audio_mode = {.channels = 2, .streams = 1, .coupled_streams = 1, .bitrate = 64000}});
  };

  auto event_bus = std::make_shared<events::EventBusType>();
  auto video_ctx = std::make_shared<immer::atom<gst_video_context::gst_context_ptr>>();
  auto no_producer = [] { return std::string(); };
  auto paused_video = std::async(std::launch::async, [=]() {
    streaming::start_streaming_video(video_session(1), no_producer, event_bus, "127.0.0.1", 1, video_ctx, nullptr);
  });
  auto stopped_video = std::async(std::launch::async, [=]() {
    streaming::start_streaming_video(video_session(2), no_producer, event_bus, "127.0.0.1", 1, video_ctx, nullptr);
  });
  auto stopped_audio = std::async(std::launch::async, [=]() {
    streaming::start_streaming_audio(audio_session(3),
                                     no_producer,
                                     event_bus,
                                     "127.0.0.1",
                                     1,
                                     nullptr,
                                     "sink",
                                     "server");
  });
  auto paused_audio = std::async(std::launch::async, [=]() {
    streaming::start_streaming_audio(audio_session(4),
                                     no_producer,
                                     event_bus,
                                     "127.0.0.1",
                                     1,
                                     nullptr,
                                     "sink",
                                     "server");
  });
  // All four are now ~1s into a 3s stuck buffer
  std::this_thread::sleep_for(4s);

  auto fire_then_register = [event_bus](auto event) {
    auto start = std::chrono::steady_clock::now();
    event_bus->fire_event(immer::box<decltype(event)>(event));
    // Needs the bus write lock, like a runner (un)registering its handlers
    auto other = event_bus->register_handler<immer::box<events::IDRRequestEvent>>([]() {});
    return std::chrono::steady_clock::now() - start;
  };
  CHECK(fire_then_register(events::IDRRequestEvent{.session_id = 1}) < 1s);
  CHECK(fire_then_register(events::PauseStreamEvent{.session_id = 1}) < 1s);
  CHECK(fire_then_register(events::StopStreamEvent{.session_id = 2}) < 1s);
  CHECK(fire_then_register(events::StopStreamEvent{.session_id = 3}) < 1s);
  CHECK(fire_then_register(events::PauseStreamEvent{.session_id = 4}) < 1s);

  // The EOS still reaches each pipeline once its streaming thread moves on, and run_pipeline returns
  REQUIRE(paused_video.wait_for(10s) == std::future_status::ready);
  REQUIRE(stopped_video.wait_for(10s) == std::future_status::ready);
  REQUIRE(stopped_audio.wait_for(10s) == std::future_status::ready);
  REQUIRE(paused_audio.wait_for(10s) == std::future_status::ready);
}

/*
 * A stop while the pipeline is still prerolling (its first buffer hasn't reached the sink) must still end it.
 * An EOS sent to the pipeline itself holds the pipeline's STATE_LOCK until the source takes it, and the preroll needs
 * that lock to finish: the pipeline would hang forever, leaking it and a GStreamer pool thread.
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "Stopping a pipeline during preroll ends it", "[GSTPlugin]") {
  using namespace wolf::core;
  using namespace std::chrono_literals;

  auto event_bus = std::make_shared<events::EventBusType>();
  // The first buffer parks the streaming thread for 2s, so the (async) sink is still prerolling when the stop comes
  auto video_session = immer::box<events::VideoSession>(
      events::VideoSession{.display_mode = {.width = 64, .height = 64, .refreshRate = 30},
                           .gst_pipeline = "videotestsrc ! identity sleep-time=2000000 ! fakesink",
                           .session_id = 1,
                           .timeout_ms = 1000,
                           .packet_size = 1024,
                           .fec_percentage = 20,
                           .min_required_fec_packets = 2,
                           .bitrate_kbps = 1000,
                           .slices_per_frame = 1,
                           .client_ip = "127.0.0.1"});
  // Not std::async: its future would block the test forever in its destructor if the pipeline hangs
  auto ended = std::make_shared<std::promise<void>>();
  auto ended_future = ended->get_future();
  std::thread([=]() {
    streaming::start_streaming_video(
        video_session,
        [] { return std::string(); },
        event_bus,
        "127.0.0.1",
        1,
        std::make_shared<immer::atom<gst_video_context::gst_context_ptr>>(),
        nullptr);
    ended->set_value();
  }).detach();
  std::this_thread::sleep_for(500ms);

  event_bus->fire_event(immer::box<events::StopStreamEvent>(events::StopStreamEvent{.session_id = 1}));
  REQUIRE(ended_future.wait_for(10s) == std::future_status::ready);
}

// XERK-2111: a pause/stop that fired after the client's ping but before the pipeline registered its own handlers
TEST_CASE_METHOD(GStreamerTestsFixture, "A pipeline whose stream already ended is never played", "[GSTPlugin]") {
  using namespace wolf::core;
  using namespace std::chrono_literals;
  // Live and never ending: once played, it would only end on an EOS, which nothing sends here
  auto audio_session = immer::box<events::AudioSession>(
      events::AudioSession{.gst_pipeline = "audiotestsrc is-live=true ! fakesink",
                           .session_id = 1,
                           .client_ip = "127.0.0.1",
                           .packet_duration = 5,
                           .audio_mode = {.channels = 2, .streams = 1, .coupled_streams = 1, .bitrate = 64000}});
  auto stopped = std::make_shared<std::atomic_bool>(true);

  // Not std::async: its future would block the test forever in its destructor if the pipeline never ends
  auto ended = std::make_shared<std::promise<void>>();
  auto ended_future = ended->get_future();
  auto event_bus = std::make_shared<events::EventBusType>();
  std::thread([=]() {
    streaming::start_streaming_audio(
        audio_session,
        [] { return std::string(); },
        event_bus,
        "127.0.0.1",
        1,
        nullptr,
        "sink",
        "",
        stopped);
    ended->set_value();
  }).detach();
  REQUIRE(ended_future.wait_for(2s) == std::future_status::ready);
  REQUIRE(event_bus->handler_count() == 0);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Stopping a consumer whose producer never sends ends it", "[GSTPlugin]") {
  using namespace wolf::core;
  // No interpipesink named nobody_audio: the source never gets a buffer (an app without an audio server)
  auto pipeline = gstreamer::gst_element_ptr(
      gst_parse_launch("interpipesrc listen-to=nobody_audio is-live=true stream-sync=restart-ts max-bytes=0 "
                       "max-buffers=3 block=false ! fakesink",
                       nullptr),
      ::gst_object_unref);
  REQUIRE(pipeline);
  gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  gstreamer::send_eos_async(pipeline.get());
  auto bus = gst_element_get_bus(pipeline.get());
  auto msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_EOS);
  REQUIRE(msg != nullptr);
  gst_message_unref(msg);
  gst_object_unref(bus);
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);
}

TEST_CASE_METHOD(GStreamerTestsFixture, "Ending a pipeline twice at once sends one EOS", "[GSTPlugin]") {
  using namespace wolf::core;
  // A pause and a stop can both end the same pipeline; two concurrent EOS events into one basesrc abort Wolf
  // inside GStreamer (gst_base_src_loop: has_pending_eos). Without the fix this usually aborts within 1000 rounds.
  for (int round = 0; round < 1000; round++) {
    auto pipeline = gstreamer::gst_element_ptr(
        gst_parse_launch("audiotestsrc is-live=true samplesperbuffer=64 ! fakesink sync=false", nullptr),
        ::gst_object_unref);
    REQUIRE(pipeline);
    gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
    gst_element_get_state(pipeline.get(), nullptr, nullptr, GST_CLOCK_TIME_NONE);

    gstreamer::send_eos_async(pipeline.get());
    gstreamer::send_eos_async(pipeline.get());
    auto bus = gst_element_get_bus(pipeline.get());
    auto msg = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, GST_MESSAGE_EOS);
    REQUIRE(msg != nullptr);
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
  }
}

/*
 * INTERPIPE
 */

/**
 * A producer's interpipesink is torn down while consumers on other threads keep joining, leaving and sending
 * upstream events to it. Unpatched gst-interpipe SIGSEGVs here within the first ~100 producer teardowns (see
 * docker/patches/gst-interpipe-teardown-race.patch).
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "Interpipe producer teardown with active consumers", "[GSTPlugin]") {
  auto interpipe = gst_element_factory_find("interpipesrc");
  REQUIRE(interpipe); // fail rather than skip, so a broken CI install doesn't read as green
  gst_object_unref(interpipe);

  constexpr int n_consumers = 16;
  constexpr int n_producers = 500;
  std::atomic<bool> stop = false;
  std::vector<std::thread> consumers;

  for (int i = 0; i < n_consumers; i++) {
    consumers.emplace_back([i, &stop]() {
      auto desc = fmt::format("interpipesrc name=teardown_c{} listen-to=teardown_prod is-live=true ! "
                              "fakesink sync=false",
                              i);
      while (!stop) {
        auto pipeline = gst_parse_launch(desc.c_str(), nullptr);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        auto src = gst_bin_get_by_name(GST_BIN(pipeline), fmt::format("teardown_c{}", i).c_str());
        auto pad = gst_element_get_static_pad(src, "src");
        for (int ev = 0; ev < 10; ev++) {
          gst_pad_send_event(pad, gst_event_new_reconfigure());
        }
        gst_object_unref(pad);
        gst_object_unref(src);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
      }
    });
  }

  for (int i = 0; i < n_producers; i++) {
    auto pipeline = gst_parse_launch("videotestsrc is-live=true ! video/x-raw,width=64,height=64 ! "
                                     "interpipesink name=teardown_prod sync=false async=false",
                                     nullptr);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::this_thread::sleep_for(std::chrono::microseconds(i % 7 * 300));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline); // finalizing the interpipesink unregisters the node
  }

  stop = true;
  for (auto &t : consumers) {
    t.join();
  }
  SUCCEED("no crash or deadlock");
}

/**
 * An upstream event forwarded from a consumer is still inside the producer (held by a pad probe) when the producer is
 * disposed: tearing the producer down must wait for that forward to return instead of freeing the node under it.
 */
TEST_CASE_METHOD(GStreamerTestsFixture, "Interpipe producer teardown waits for forwarded events", "[GSTPlugin]") {
  auto interpipe = gst_element_factory_find("interpipesrc");
  REQUIRE(interpipe);
  gst_object_unref(interpipe);

  static std::atomic<bool> in_probe = false;
  auto consumer =
      gst_parse_launch("interpipesrc name=fwd_c listen-to=fwd_prod is-live=true ! fakesink sync=false", nullptr);
  gst_element_set_state(consumer, GST_STATE_PLAYING);
  auto consumer_src = gst_bin_get_by_name(GST_BIN(consumer), "fwd_c");
  auto consumer_pad = gst_element_get_static_pad(consumer_src, "src");

  auto producer = gst_parse_launch(
      "videotestsrc is-live=true ! video/x-raw,width=64,height=64 ! identity name=fwd_id ! "
      "interpipesink name=fwd_prod sync=false async=false",
      nullptr);
  auto identity = gst_bin_get_by_name(GST_BIN(producer), "fwd_id");
  auto identity_pad = gst_element_get_static_pad(identity, "src");
  gst_pad_add_probe(
      identity_pad,
      GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
      [](GstPad *, GstPadProbeInfo *info, gpointer) {
        if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_CUSTOM_UPSTREAM) {
          in_probe = true;
          std::this_thread::sleep_for(std::chrono::milliseconds(200));
          in_probe = false;
        }
        return GST_PAD_PROBE_OK;
      },
      nullptr,
      nullptr);
  gst_object_unref(identity_pad);
  gst_object_unref(identity);
  gst_element_set_state(producer, GST_STATE_PLAYING);
  std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the consumer connect

  std::thread sender([consumer_pad]() {
    gst_pad_send_event(consumer_pad,
                       gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, gst_structure_new_empty("wolf-test")));
  });
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!in_probe && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(in_probe);

  gst_element_set_state(producer, GST_STATE_NULL);
  gst_object_unref(producer); // disposing the interpipesink unregisters the node
  REQUIRE_FALSE(in_probe);    // the forward finished before the node was torn down

  sender.join();
  gst_element_set_state(consumer, GST_STATE_NULL);
  gst_object_unref(consumer_pad);
  gst_object_unref(consumer_src);
  gst_object_unref(consumer);
}
