#pragma once

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <helpers/logger.hpp>

namespace wolf::core::gstreamer {

using gst_element_ptr = std::shared_ptr<GstElement>;
using gst_main_loop_ptr = std::shared_ptr<GMainLoop>;
using gst_main_context_ptr = std::shared_ptr<GMainContext>;

static void pipeline_error_handler(GstBus *bus, GstMessage *message, gpointer data) {
  auto loop = (GMainLoop *)data;
  GError *err;
  gchar *debug;
  gst_message_parse_error(message, &err, &debug);
  logs::log(logs::error, "[GSTREAMER] Pipeline error: {}", err->message);
  g_error_free(err);
  g_free(debug);

  /* Terminate pipeline on error */
  g_main_loop_quit(loop);
}

static void pipeline_eos_handler(GstBus *bus, GstMessage *message, gpointer data) {
  auto loop = (GMainLoop *)data;
  logs::log(logs::info, "[GSTREAMER] Pipeline reached End Of Stream");
  g_main_loop_quit(loop);
}

/**
 * Sends a custom message in the pipeline
 */
static void send_message(GstElement *recipient, GstStructure *message) {
  if (!recipient) {
    // Avoid throwing errors when we don't have a recipient
    // This might happen when we have the control stream connected, but the wayland display isn't ready yet
    return;
  }
  auto gst_ev = gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, message);
  gst_element_send_event(recipient, gst_ev);
}

/**
 * Like send_message, from GStreamer's own thread pool: returns immediately (see send_eos_async).
 */
static void send_message_async(GstElement *recipient, GstStructure *message) {
  if (!recipient) {
    gst_structure_free(message);
    return;
  }
  gst_element_call_async(
      recipient,
      [](GstElement *element, gpointer message) { send_message(element, static_cast<GstStructure *>(message)); },
      message,
      nullptr);
}

/**
 * Sends EOS to the pipeline from GStreamer's own thread pool and returns immediately.
 *
 * A synchronous gst_element_send_event(EOS) blocks on the source's streaming lock, so a stuck
 * streaming thread hangs the caller too. Event-bus handlers run while the bus holds its lock,
 * which means a blocked handler stalls every register/unregister in the whole server.
 * gst_element_call_async keeps a ref on the element until the call has run.
 *
 * The EOS goes to each source, not to the pipeline: sending it to a bin holds the bin's STATE_LOCK
 * until the sources have taken it, and a pipeline still prerolling needs that lock to finish its
 * state change, so a stop during preroll would deadlock the pipeline forever.
 *
 * An appsrc (interpipesrc is one) gets gst_app_src_end_of_stream instead: an EOS event only reaches
 * its streaming thread with the next buffer, so a consumer whose producer never sends one (an app
 * without an audio server) would never end.
 */
static void send_eos_async(GstElement *pipeline) {
  gst_element_call_async(
      pipeline,
      [](GstElement *element, gpointer) {
        if (!GST_IS_BIN(element)) {
          gst_element_send_event(element, gst_event_new_eos());
          return;
        }
        GstIterator *sources = gst_bin_iterate_sources(GST_BIN(element));
        GValue source = G_VALUE_INIT;
        bool sent = false;
        // A source added mid-iteration (GST_ITERATOR_RESYNC) is ignored: Wolf's pipelines are static
        while (gst_iterator_next(sources, &source) == GST_ITERATOR_OK) {
          auto src = GST_ELEMENT(g_value_get_object(&source));
          if (GST_IS_APP_SRC(src)) {
            gst_app_src_end_of_stream(GST_APP_SRC(src));
          } else {
            gst_element_send_event(src, gst_event_new_eos());
          }
          sent = true;
          g_value_reset(&source);
        }
        g_value_unset(&source);
        gst_iterator_free(sources);
        if (!sent) {
          gst_element_send_event(element, gst_event_new_eos());
        }
      },
      nullptr,
      nullptr);
}

/**
 * Given a Gstreamer element returns the supported DRM formats (if any).
 * Ex: "vah265enc" -> ["P010:0x0200000000042305", "NV12:0x0200000000042305"]
 */
static std::vector<std::string> get_dma_caps(const std::string &gst_plugin_name) {
  std::vector<std::string> caps;
  GstRegistry *registry = gst_registry_get();
  if (auto feature = gst_registry_find_feature(registry, gst_plugin_name.c_str(), GST_TYPE_ELEMENT_FACTORY)) {
    if (auto real_feature = gst_registry_lookup_feature(gst_registry_get(), GST_OBJECT_NAME(feature))) {
      auto pads = gst_element_factory_get_static_pad_templates(GST_ELEMENT_FACTORY(real_feature));
      for (auto pad = pads; pad; pad = g_list_next(pad)) {
        auto pad_template = (GstStaticPadTemplate *)(pad->data);
        if (pad_template->static_caps.string && pad_template->direction == GST_PAD_SINK) {
          GstCaps *current_caps = gst_static_caps_get(&pad_template->static_caps);
          // iterate over caps looking for the type memory:DMABuf
          gst_caps_foreach(
              current_caps,
              [](GstCapsFeatures *features, GstStructure *structure, gpointer user_data) -> gboolean {
                auto caps = (std::vector<std::string> *)user_data;
                if (features && gst_caps_features_contains(features, "memory:DMABuf")) {
                  // get the array of supported formats under "drm-format" (if present)
                  GValueArray *formats = nullptr;
                  gst_structure_get_list(structure, "drm-format", &formats);
                  if (formats) {
                    for (guint i = 0; i < formats->n_values; i++) {
                      GValue *format = &formats->values[i];
                      if (G_VALUE_HOLDS_STRING(format)) {
                        caps->push_back(g_value_get_string(format));
                      }
                    }
                  }
                }
                return true;
              },
              &caps);
          gst_caps_unref(current_caps);
        }
      }
      gst_object_unref(real_feature);
    }
    gst_object_unref(feature);
  }

  return caps;
}
} // namespace wolf::core::gstreamer