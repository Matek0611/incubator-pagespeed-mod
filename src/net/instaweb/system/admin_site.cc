// Copyright 2013 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: jefftk@google.com (Jeff Kaufman)
// Author: xqyin@google.com (XiaoQian Yin)

#include "net/instaweb/system/public/admin_site.h"

#include <cstddef>
#include <memory>
#include <set>
#include <vector>

#include "net/instaweb/http/public/async_fetch.h"
#include "net/instaweb/http/public/http_cache.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/rewrite_query.h"
#include "net/instaweb/rewriter/public/server_context.h"
#include "net/instaweb/system/public/system_caches.h"
#include "net/instaweb/system/public/system_cache_path.h"
#include "net/instaweb/system/public/system_rewrite_options.h"
#include "net/instaweb/util/public/message_handler.h"
#include "net/instaweb/util/public/property_cache.h"
#include "net/instaweb/util/public/property_store.h"
#include "net/instaweb/util/public/query_params.h"
#include "net/instaweb/util/public/statistics.h"
#include "net/instaweb/util/public/statistics_logger.h"
#include "net/instaweb/util/public/writer.h"
#include "pagespeed/kernel/base/cache_interface.h"
#include "pagespeed/kernel/base/callback.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/base/string_writer.h"
#include "pagespeed/kernel/base/timer.h"
#include "pagespeed/kernel/cache/purge_context.h"
#include "pagespeed/kernel/html/html_keywords.h"
#include "pagespeed/kernel/http/content_type.h"
#include "pagespeed/kernel/http/google_url.h"
#include "pagespeed/kernel/http/http_names.h"
#include "pagespeed/kernel/http/request_headers.h"
#include "pagespeed/kernel/http/response_headers.h"

namespace net_instaweb {

// Generated from JS, CSS, and HTML source via net/instaweb/js/data_to_c.cc.
extern const char* CSS_console_css;
extern const char* CSS_mod_pagespeed_console_css;
extern const char* HTML_mod_pagespeed_console_body;
extern const char* JS_caches_js;
extern const char* JS_caches_js_opt;
extern const char* JS_console_js;
extern const char* JS_console_js_opt;
extern const char* JS_graphs_js;
extern const char* JS_graphs_js_opt;
extern const char* JS_messages_js;
extern const char* JS_messages_js_opt;
extern const char* JS_mod_pagespeed_console_js;
extern const char* JS_statistics_js;
extern const char* JS_statistics_js_opt;

namespace {

// This style fragment is copied from ../rewriter/console.css because it's
// kind of nice.  However if we import the whole console.css into admin pages
// it looks terrible.
//
// TODO(jmarantz): Get UX help to style the whole admin site better.
// TODO(jmarantz): Factor this out into its own CSS file.
const char kATagStyle[] =
    "a {text-decoration:none; color:#15c; cursor:pointer;}"
    "a:visited {color: #61c;}"
    "a:hover {text-decoration:underline;}"
    "a:active {text-decoration:underline; color:#d14836;}";

struct Tab {
  const char* label;
  const char* title;
  const char* admin_link;       // relative from /pagespeed_admin/
  const char* statistics_link;  // relative from /mod_pagespeed_statistics
  const char* space;            // html for inter-link spacing.
};

const char kShortBreak[] = " ";
const char kLongBreak[] = " &nbsp;&nbsp; ";

// TODO(jmarantz): disable or recolor links to pages that are not available
// based on the current config.
const Tab kTabs[] = {
  {"Statistics", "Statistics", "statistics", "?", kLongBreak},
  {"Configuration", "Configuration", "config", "?config", kShortBreak},
  {"(SPDY)", "SPDY Configuration", "spdy_config", "?spdy_config", kLongBreak},
  {"Histograms", "Histograms", "histograms", "?histograms", kLongBreak},
  {"Caches", "Caches", "cache", "?cache", kLongBreak},
  {"Console", "Console", "console", NULL, kLongBreak},
  {"Message History", "Message History", "message_history", NULL, kLongBreak},
  {"Graphs", "Graphs", "graphs", NULL, kLongBreak},
};

// Controls the generation of an HTML Admin page.  Constructing it
// establishes the content-type as HTML and response code 200, and
// puts in a banner with links to all the admin pages, ready for
// appending more <body> elements.  Destructing AdminHtml closes the
// body and completes the fetch.
class AdminHtml {
 public:
  AdminHtml(StringPiece current_link, StringPiece head_extra,
            AdminSite::AdminSource source, AsyncFetch* fetch,
            MessageHandler* handler)
      : fetch_(fetch),
        handler_(handler) {
    fetch->response_headers()->SetStatusAndReason(HttpStatus::kOK);
    fetch->response_headers()->Add(HttpAttributes::kContentType, "text/html");

    // Let PageSpeed dynamically minify the html, css, and javasript
    // generated by the admin page, to the extent it's not done
    // already by the tools.  Note, this does mean that viewing the
    // statistics and histograms pages will affect the statistics and
    // histograms.  If we decide this is too annoying, then we can
    // instead procedurally minify the css/js and leave the html
    // alone.
    //
    // Note that we at least turn off add_instrumenation here by explicitly
    // giving a filter list without "+" or "-".
    fetch->response_headers()->Add(
        RewriteQuery::kPageSpeedFilters,
        "rewrite_css,rewrite_javascript,collapse_whitespace");

    // Generate some navigational links to help our users get to other
    // admin pages.
    fetch->Write("<!DOCTYPE html>\n<html><head>", handler_);
    fetch->Write(StrCat("<style>", kATagStyle, "</style>"), handler_);

    GoogleString buf;
    for (int i = 0, n = arraysize(kTabs); i < n; ++i) {
      const Tab& tab = kTabs[i];
      const char* link = NULL;
      switch (source) {
        case AdminSite::kPageSpeedAdmin:
          link = tab.admin_link;
          break;
        case AdminSite::kStatistics:
          link = tab.statistics_link;
          break;
        case AdminSite::kOther:
          link = NULL;
          break;
      }
      if (link != NULL) {
        StringPiece style;
        if (tab.admin_link == current_link) {
          style = " style='color:darkblue;text-decoration:underline;'";
          fetch->Write(StrCat("<title>PageSpeed ", tab.title, "</title>"),
                       handler_);
        }
        StrAppend(&buf,
                  "<a href='", link, "'", style, ">", tab.label, "</a>",
                  tab.space);
      }
    }

    fetch->Write(StrCat(head_extra, "</head>"), handler_);
    fetch->Write(
        StrCat("<body><div style='font-size:16px;font-family:sans-serif;'>\n"
               "<b>Pagespeed Admin</b>", kLongBreak, "\n"),
        handler_);
    fetch->Write(buf, handler_);
    fetch->Write("</div><hr/>\n", handler_);
    fetch->Flush(handler_);
  }

  ~AdminHtml() {
    fetch_->Write("</body></html>", handler_);
    fetch_->Done(true);
  }

 private:
  AsyncFetch* fetch_;
  MessageHandler* handler_;
};

}  // namespace

AdminSite::AdminSite(StaticAssetManager* static_asset_manager, Timer* timer,
                     MessageHandler* message_handler)
    : message_handler_(message_handler),
      static_asset_manager_(static_asset_manager),
      timer_(timer) {
}

// Handler which serves PSOL console.
void AdminSite::ConsoleHandler(const SystemRewriteOptions& global_options,
                               const RewriteOptions& options,
                               AdminSource source,
                               const QueryParams& query_params,
                               AsyncFetch* fetch, Statistics* statistics) {
  if (query_params.Has("json")) {
    ConsoleJsonHandler(query_params, fetch, statistics);
    return;
  }

  MessageHandler* handler = message_handler_;
  bool statistics_enabled = global_options.statistics_enabled();
  bool logging_enabled = global_options.statistics_logging_enabled();
  bool log_dir_set = !global_options.log_dir().empty();

  // TODO(jmarantz): change StaticAssetManager to take options by const ref.
  // TODO(sligocki): Move static content to a data2cc library.
  StringPiece console_js = options.Enabled(RewriteOptions::kDebug) ?
      JS_console_js :
      JS_console_js_opt;
  // TODO(sligocki): Do we want to have a minified version of console CSS?
  GoogleString head_markup = StrCat(
      "<style>", CSS_console_css, "</style>\n");
  AdminHtml admin_html("console", head_markup, source, fetch,
                       message_handler_);
  if (statistics_enabled && logging_enabled && log_dir_set) {
    fetch->Write("<div class='console_div' id='suggestions'>\n"
                 "  <div class='console_div' id='pagespeed-graphs-container'>"
                 "</div>\n</div>\n"
                 "<script src='https://www.google.com/jsapi'></script>\n"
                 "<script>var pagespeedStatisticsUrl = '';</script>\n"
                 "<script>", handler);
    // From the admin page, the console JSON is relative, so it can
    // be set to ''.  Formerly it was set to options.statistics_handler_path(),
    // but there does not appear to be a disadvantage to always handling it
    // from whatever URL served this console HTML.
    //
    // TODO(jmarantz): Change the JS to remove pagespeedStatisticsUrl.
    fetch->Write(console_js, handler);
    fetch->Write("</script>\n", handler);
  } else {
    fetch->Write("<p>\n"
                 "  Failed to load PageSpeed Console because:\n"
                 "</p>\n"
                 "<ul>\n", handler);
    if (!statistics_enabled) {
      fetch->Write("  <li>Statistics is not enabled.</li>\n",
                    handler);
    }
    if (!logging_enabled) {
      fetch->Write("  <li>StatisticsLogging is not enabled."
                    "</li>\n", handler);
    }
    if (!log_dir_set) {
      fetch->Write("  <li>LogDir is not set.</li>\n", handler);
    }
    fetch->Write("</ul>\n"
                  "<p>\n"
                  "  In order to use the console you must configure these\n"
                  "  options. See the <a href='https://developers.google.com/"
                  "speed/pagespeed/module/console'>console documentation</a>\n"
                  "  for more details.\n"
                  "</p>\n", handler);
  }
}

// TODO(sligocki): integrate this into the pagespeed_console.
void AdminSite::StatisticsGraphsHandler(
    Writer* writer, SystemRewriteOptions* global_system_rewrite_options) {
  SystemRewriteOptions* options = global_system_rewrite_options;
  writer->Write("<!DOCTYPE html>"
                "<title>mod_pagespeed console</title>",
                message_handler_);
  writer->Write("<style>", message_handler_);
  writer->Write(CSS_mod_pagespeed_console_css, message_handler_);
  writer->Write("</style>", message_handler_);
  writer->Write(HTML_mod_pagespeed_console_body, message_handler_);
  writer->Write("<script>", message_handler_);
  if (options->statistics_logging_charts_js().size() > 0 &&
      options->statistics_logging_charts_css().size() > 0) {
    writer->Write("var chartsOfflineJS = '", message_handler_);
    writer->Write(options->statistics_logging_charts_js(), message_handler_);
    writer->Write("';", message_handler_);
    writer->Write("var chartsOfflineCSS = '", message_handler_);
    writer->Write(options->statistics_logging_charts_css(), message_handler_);
    writer->Write("';", message_handler_);
  } else {
    if (options->statistics_logging_charts_js().size() > 0 ||
        options->statistics_logging_charts_css().size() > 0) {
      message_handler_->Message(kWarning, "Using online Charts API.");
    }
    writer->Write("var chartsOfflineJS, chartsOfflineCSS;", message_handler_);
  }
  writer->Write(JS_mod_pagespeed_console_js, message_handler_);
  writer->Write("</script>", message_handler_);
}

void AdminSite::StatisticsHandler(const RewriteOptions& options,
                                  AdminSource source, AsyncFetch* fetch,
                                  Statistics* stats) {
  AdminHtml admin_html("statistics", "", source, fetch, message_handler_);
  // Write <pre></pre> for Dump to keep good format.
  fetch->Write("<pre id=\"stat\">", message_handler_);
  stats->Dump(fetch, message_handler_);
  fetch->Write("</pre>\n", message_handler_);
  StringPiece statistics_js = options.Enabled(RewriteOptions::kDebug) ?
        JS_statistics_js :
        JS_statistics_js_opt;
  fetch->Write(StrCat("<script type=\"text/javascript\">", statistics_js,
                      "\npagespeed.Statistics.Start();</script>\n"),
               message_handler_);
}

void AdminSite::GraphsHandler(const RewriteOptions& options,
                              AdminSource source, AsyncFetch* fetch,
                              Statistics* stats) {
  AdminHtml admin_html("graphs", "", source, fetch, message_handler_);
  fetch->Write("<div id=\"cache_applied\"></div>"
               "<div id=\"cache_type\" style=\"display:none\"></div>"
               "<div id=\"ipro\" style=\"display:none\"></div>"
               "<div id=\"image_rewriting\" style=\"display:none\"></div>"
               "<div id=\"realtime\" style=\"display:none\"></div>",
               message_handler_);
  fetch->Write("<script type=\"text/javascript\" "
               "src=\"https://www.google.com/jsapi\"></script>",
               message_handler_);
  StringPiece graphs_js = options.Enabled(RewriteOptions::kDebug) ?
        JS_graphs_js :
        JS_graphs_js_opt;
  fetch->Write(StrCat("<script type=\"text/javascript\">", graphs_js,
                      "\npagespeed.Graphs.Start();</script>\n"),
               message_handler_);
}

void AdminSite::ConsoleJsonHandler(const QueryParams& params,
                                   AsyncFetch* fetch, Statistics* statistics) {
  StatisticsLogger* console_logger = statistics->console_logger();
  if (console_logger == NULL) {
    fetch->response_headers()->SetStatusAndReason(HttpStatus::kNotFound);
    fetch->response_headers()->Add(HttpAttributes::kContentType, "text/plain");
    fetch->Write(
        "console_logger must be enabled to use '?json' query parameter.",
        message_handler_);
  } else {
    fetch->response_headers()->SetStatusAndReason(HttpStatus::kOK);

    fetch->response_headers()->Add(HttpAttributes::kContentType,
                                   kContentTypeJson.mime_type());

    int64 start_time, end_time, granularity_ms;
    std::set<GoogleString> var_titles;

    // Default values for start_time, end_time, and granularity_ms in case the
    // query does not include these parameters.
    start_time = 0;
    end_time = timer_->NowMs();
    // Granularity is the difference in ms between data points. If it is not
    // specified by the query, the default value is 3000 ms, the same as the
    // default logging granularity.
    granularity_ms = 3000;
    for (int i = 0; i < params.size(); ++i) {
      GoogleString value;
      if (params.UnescapedValue(i, &value)) {
        StringPiece name = params.name(i);
        if (name =="start_time") {
          StringToInt64(value, &start_time);
        } else if (name == "end_time") {
          StringToInt64(value, &end_time);
        } else if (name == "var_titles") {
          std::vector<StringPiece> variable_names;
          SplitStringPieceToVector(value, ",", &variable_names, true);
          for (size_t i = 0; i < variable_names.size(); ++i) {
            var_titles.insert(variable_names[i].as_string());
          }
        } else if (name == "granularity") {
          StringToInt64(value, &granularity_ms);
        }
      }
    }
    console_logger->DumpJSON(var_titles, start_time, end_time, granularity_ms,
                             fetch, message_handler_);
  }
  fetch->Done(true);
}

void AdminSite::PrintHistograms(AdminSource source, AsyncFetch* fetch,
                                Statistics* stats) {
  AdminHtml admin_html("histograms", "", source, fetch, message_handler_);
  stats->RenderHistograms(fetch, message_handler_);
}

namespace {

static const char kBackToPurgeCacheButton[] =
    "<br><input type=\"button\" value=\"Back\" "
    "onclick=\"location.href='./cache#purge_cache'\"/>";

static const char kTableStart[] =
    "<table style='font-family:sans-serif;font-size:0.9em'>\n"
    "  <thead>\n"
    "    <tr style='font-weight:bold'>\n"
    "      <td>Cache</td><td>Detail</td><td>Structure</td>\n"
    "    </tr>\n"
    "  </thead>\n"
    "  <tbody>";

static const char kTableEnd[] =
    "  </tbody>\n"
    "</table>";

// Takes a complicated descriptor like
//    "HTTPCache(Fallback(small=Batcher(cache=Stats(parefix=memcached_async,"
//    "cache=Async(AprMemCache)),parallelism=1,max=1000),large=Stats("
//    "prefix=file_cache,cache=FileCache)))"
// and strips away the crap most users don't want to see, as they most
// likely did not configure it, and return
//    "Async AprMemCache FileCache"
GoogleString HackCacheDescriptor(StringPiece name) {
  GoogleString out;
  // There's a lot of complicated syntax in the cache name giving the
  // detailed hierarchical structure.  This is really hard to read and
  // overly cryptic; it's designed for unit tests.  But let's extract
  // a few keywords out of this to understand the main pointers.
  static const char* kCacheKeywords[] = {
    "Compressed", "Async", "SharedMemCache", "LRUCache", "AprMemCache",
    "FileCache"
  };
  const char* delim = "";
  for (int i = 0, n = arraysize(kCacheKeywords); i < n; ++i) {
    if (name.find(kCacheKeywords[i]) != StringPiece::npos) {
      StrAppend(&out, delim, kCacheKeywords[i]);
      delim = " ";
    }
  }
  if (out.empty()) {
    name.CopyToString(&out);
  }
  return out;
}

// Takes a complicated descriptor like
//    "HTTPCache(Fallback(small=Batcher(cache=Stats(prefix=memcached_async,"
//    "cache=Async(AprMemCache)),parallelism=1,max=1000),large=Stats("
//    "prefix=file_cache,cache=FileCache)))"
// and injects HTML line-breaks and indentation based on the parent depth,
// yielding HTML that renders like this (with &nbsp; and <br/>)
//    HTTPCache(
//       Fallback(
//          small=Batcher(
//             cache=Stats(
//                prefix=memcached_async,
//                cache=Async(
//                   AprMemCache)),
//             parallelism=1,
//             max=1000),
//          large=Stats(
//             prefix=file_cache,
//             cache=FileCache)))
GoogleString IndentCacheDescriptor(StringPiece name) {
  GoogleString out, buf;
  int depth = 0;
  for (int i = 0, n = name.size(); i < n; ++i) {
    StrAppend(&out, HtmlKeywords::Escape(name.substr(i, 1), &buf));
    switch (name[i]) {
      case '(':
        ++depth;
        FALLTHROUGH_INTENDED;
      case ',':
        out += "<br/>";
        for (int j = 0; j < depth; ++j) {
          out += "&nbsp; &nbsp;";
        }
        break;
      case ')':
        --depth;
        break;
    }
  }
  return out;
}

GoogleString CacheInfoHtmlSnippet(StringPiece label, StringPiece descriptor) {
  GoogleString out, escaped;
  StrAppend(&out, "<tr style=\"vertical-align:top;\"><td>", label,
            "</td><td><input id=\"", label);
  StrAppend(&out, "_toggle\" type=\"checkbox\" ",
            "onclick=\"pagespeed.Caches.toggleDetail('", label,
            "')\"/></td><td><code id=\"", label, "_summary\">");
  StrAppend(&out, HtmlKeywords::Escape(HackCacheDescriptor(descriptor),
                                       &escaped));
  StrAppend(&out, "</code><code id=\"", label,
            "_detail\" style=\"display:none;\">");
  StrAppend(&out, IndentCacheDescriptor(descriptor));
  StrAppend(&out, "</code></td></tr>\n");
  return out;
}

// Returns an HTML form for entering a URL for ShowCacheHandler.  If
// the user_agent is non-null, then it's used to prepopulate the
// "User Agent" field in the form.
GoogleString ShowCacheForm(const char* user_agent) {
  GoogleString ua_default;
  if (user_agent != NULL) {
    GoogleString buf;
    ua_default = StrCat("value=\"", HtmlKeywords::Escape(user_agent, &buf),
                        "\" ");
  }
  // The styling on this form could use some love, but the 110/103 sizing
  // is to make those input fields decently wide to fit large URLs and UAs
  // and to roughly line up.
  GoogleString out = StrCat(
      "<form method=get>\n",
      "  URL: <input type=text name=url size=110 /><br>\n"
      "  User-Agent: <input type=text size=103 name=user_agent ",
      ua_default,
      "/></br> \n",
      "   <input type=submit value='Show Metadata Cache Entry'/>"
      "</form>\n");
  return out;
}

}  // namespace

void AdminSite::PrintCaches(bool is_global, AdminSource source,
                            const GoogleUrl& stripped_gurl,
                            const QueryParams& query_params,
                            const RewriteOptions* options,
                            SystemCachePath* cache_path,
                            AsyncFetch* fetch, SystemCaches* system_caches,
                            CacheInterface* filesystem_metadata_cache,
                            HTTPCache* http_cache,
                            CacheInterface* metadata_cache,
                            PropertyCache* page_property_cache,
                            ServerContext* server_context) {
  GoogleString url;
  if ((source == kPageSpeedAdmin) &&
      query_params.Lookup1Unescaped("url", &url)) {
    // Delegate to ShowCacheHandler to get the cached value for that
    // URL, which it may do asynchronously, so we cannot use the
    // AdminHtml abstraction which closes the connection in its
    // destructor.
    // TODO(xqyin): Figure out where the ShowCacheHandler should live to
    // eliminate the dependency here.
    server_context->ShowCacheHandler(url, fetch, options->Clone());
  } else if ((source == kPageSpeedAdmin) &&
             query_params.Lookup1Unescaped("purge", &url)) {
    ResponseHeaders* response_headers = fetch->response_headers();
    if (!options->enable_cache_purge()) {
      response_headers->SetStatusAndReason(HttpStatus::kNotFound);
      response_headers->Add(HttpAttributes::kContentType, "text/html");
      // TODO(jmarantz): virtualize the formatting of this message so that
      // it's correct in ngx_pagespeed and mod_pagespeed (and IISpeed etc).
      fetch->Write(StrCat("Purging not enabled: please add\n"
                          "<pre>\n"
                          "    PagespeedEnableCachePurge on\n"
                          "<pre>\n"
                          "to your config\n", kBackToPurgeCacheButton),
                   message_handler_);
      fetch->Done(true);
    } else if (url == "*") {
      PurgeHandler(url, cache_path, fetch);
    } else if (url.empty()) {
      response_headers->SetStatusAndReason(HttpStatus::kNotFound);
      response_headers->Add(HttpAttributes::kContentType, "text/html");
      fetch->Write(StrCat("Empty URL", kBackToPurgeCacheButton),
                   message_handler_);
      fetch->Done(true);
    } else {
      GoogleUrl origin(stripped_gurl.Origin());
      GoogleUrl resolved(origin, url);
      if (!resolved.IsWebValid()) {
        response_headers->SetStatusAndReason(HttpStatus::kNotFound);
        response_headers->Add(HttpAttributes::kContentType, "text/html");
        GoogleString escaped_url;
        HtmlKeywords::Escape(url, &escaped_url);
        fetch->Write(StrCat("Invalid URL: ", escaped_url,
                            kBackToPurgeCacheButton),
                     message_handler_);
        fetch->Done(true);
      } else {
        PurgeHandler(resolved.Spec(), cache_path, fetch);
      }
    }
  } else {
    AdminHtml admin_html("cache", "", source, fetch, message_handler_);

    fetch->Write("<div id=\"show_metadata\">", message_handler_);
    // Present a small form to enter a URL.
    if (source == kPageSpeedAdmin) {
      const char* user_agent = fetch->request_headers()->Lookup1(
          HttpAttributes::kUserAgent);
      fetch->Write(ShowCacheForm(user_agent), message_handler_);
    }
    fetch->Write("</div>\n", message_handler_);
    // Display configured cache information.
    if (system_caches != NULL) {
      int flags = SystemCaches::kDefaultStatFlags;
      if (is_global) {
        flags |= SystemCaches::kGlobalView;
      }

      // TODO(jmarantz): Consider whether it makes sense to disable
      // either of these flags to limit the content when someone asks
      // for info about the cache.
      flags |= SystemCaches::kIncludeMemcached;
      fetch->Write("<div id=\"cache_struct\" style=\"display:none\">",
                   message_handler_);
      fetch->Write(kTableStart, message_handler_);
      CacheInterface* fsmdc = filesystem_metadata_cache;
      fetch->Write(StrCat(
          CacheInfoHtmlSnippet("HTTP Cache", http_cache->Name()),
          CacheInfoHtmlSnippet("Metadata Cache", metadata_cache->Name()),
          CacheInfoHtmlSnippet("Property Cache",
                               page_property_cache->property_store()->Name()),
          CacheInfoHtmlSnippet("FileSystem Metadata Cache",
                               (fsmdc == NULL) ? "none" : fsmdc->Name())),
                   message_handler_);
      fetch->Write(kTableEnd, message_handler_);

      GoogleString backend_stats;
      system_caches->PrintCacheStats(
          static_cast<SystemCaches::StatFlags>(flags), &backend_stats);
      if (!backend_stats.empty()) {
        HtmlKeywords::WritePre(backend_stats, "", fetch, message_handler_);
      }
      fetch->Write("</div>", message_handler_);

      fetch->Write("<div id=\"purge_cache\" style=\"display:none\">",
                   message_handler_);
      fetch->Write("<h3>Purge Set</h3>", message_handler_);
      HtmlKeywords::WritePre(options->PurgeSetString(), "", fetch,
                             message_handler_);
      fetch->Write("</div>", message_handler_);
    }
    StringPiece caches_js = options->Enabled(RewriteOptions::kDebug) ?
        JS_caches_js :
        JS_caches_js_opt;
    // Practice what we preach: put the blocking JS in the tail.
    // TODO(jmarantz): use static asset manager to compile & deliver JS
    // externally.
    fetch->Write(StrCat("<script type=\"text/javascript\">", caches_js,
                        "\npagespeed.Caches.Start();</script>\n"),
                 message_handler_);
  }
}

void AdminSite::PrintNormalConfig(
    AdminSource source, AsyncFetch* fetch,
    SystemRewriteOptions* global_system_rewrite_options) {
  AdminHtml admin_html("config", "", source, fetch, message_handler_);
  HtmlKeywords::WritePre(
      global_system_rewrite_options->OptionsToString(), "",
      fetch, message_handler_);
}

void AdminSite::PrintSpdyConfig(AdminSource source, AsyncFetch* fetch,
                                const SystemRewriteOptions* spdy_config) {
  AdminHtml admin_html("spdy_config", "", source, fetch, message_handler_);
  if (spdy_config == NULL) {
    fetch->Write("SPDY-specific configuration missing.", message_handler_);
  } else {
    HtmlKeywords::WritePre(spdy_config->OptionsToString(), "",
                           fetch, message_handler_);
  }
}

void AdminSite::MessageHistoryHandler(const RewriteOptions& options,
                                      AdminSource source, AsyncFetch* fetch) {
  // Request for page /mod_pagespeed_message.
  GoogleString log;
  StringWriter log_writer(&log);
  AdminHtml admin_html("message_history", "", source, fetch, message_handler_);
  if (message_handler_->Dump(&log_writer)) {
    fetch->Write("<div id=\"log\">", message_handler_);
    // Write pre-tag and color messages.
    StringPieceVector messages;
    message_handler_->ParseMessageDumpIntoMessages(log, &messages);
    for (int i = 0, size = messages.size(); i < size; ++i) {
      if (messages[i].length() > 0) {
        switch (message_handler_->GetMessageType(messages[i])) {
          case kError: {
            HtmlKeywords::WritePre(
                message_handler_->ReformatMessage(messages[i]),
                "color:red; margin:0;", fetch, message_handler_);
            break;
          }
          case kWarning: {
            HtmlKeywords::WritePre(
                message_handler_->ReformatMessage(messages[i]),
                "color:blue; margin:0;", fetch, message_handler_);
            break;
          }
          case kFatal: {
            HtmlKeywords::WritePre(
                message_handler_->ReformatMessage(messages[i]),
                "color:orange; margin:0;", fetch, message_handler_);
            break;
          }
          default: {
            HtmlKeywords::WritePre(
                message_handler_->ReformatMessage(messages[i]),
                "margin:0;", fetch, message_handler_);
          }
        }
      }
    }
    fetch->Write("</div>\n", message_handler_);
    StringPiece messages_js = options.Enabled(RewriteOptions::kDebug) ?
        JS_messages_js :
        JS_messages_js_opt;
    fetch->Write(StrCat("<script type=\"text/javascript\">", messages_js,
                        "\npagespeed.Messages.Start();</script>\n"),
                 message_handler_);
  } else {
    fetch->Write("<p>Writing to mod_pagespeed_message failed. \n"
                 "Please check if it's enabled in pagespeed.conf.</p>\n",
                 message_handler_);
  }
}

void AdminSite::AdminPage(
    bool is_global, const GoogleUrl& stripped_gurl,
    const QueryParams& query_params, const RewriteOptions* options,
    SystemCachePath* cache_path, AsyncFetch* fetch, SystemCaches* system_caches,
    CacheInterface* filesystem_metadata_cache, HTTPCache* http_cache,
    CacheInterface* metadata_cache, PropertyCache* page_property_cache,
    ServerContext* server_context, Statistics* statistics, Statistics* stats,
    SystemRewriteOptions* global_system_rewrite_options,
    const SystemRewriteOptions* spdy_config) {
  // The handler is "pagespeed_admin", so we must dispatch off of
  // the remainder of the URL.  For
  // "http://example.com/pagespeed_admin/foo?a=b" we want to pull out
  // "foo".
  //
  // Note that the comments here referring to "/pagespeed_admin" reflect
  // only the default admin path in Apache for fresh installs.  In fact
  // we can put the handler on any path, and this code should still work;
  // all the paths here are specified relative to the incoming URL.
  StringPiece path = stripped_gurl.PathSansQuery();   // "/pagespeed_admin/foo"
  path = path.substr(1);                              // "pagespeed_admin/foo"

  // If there are no slashes at all in the path, e.g. it's "pagespeed_admin",
  // then the relative references to "config" etc will not work.  We need
  // to serve the admin pages on "/pagespeed_admin/".  So if we got to this
  // point and there are no slashes, then we can just redirect immediately
  // by adding a slash.
  //
  // If the user has mapped the pagespeed_admin handler to a path with
  // an embbedded slash, say "pagespeed/myadmin", then it's hard to tell
  // whether we should redirect, because we don't know what the the
  // intended path is.  In this case, we'll fall through to a leaf
  // analysis on "myadmin", fail to find a match, and print a "Did You Mean"
  // page.  It's not as good as a redirect but since we can't tell an
  // omitted slash from a typo it's the best we can do.
  if (path.find('/') == StringPiece::npos) {
    // If the URL is "/pagespeed_admin", then redirect to "/pagespeed_admin/" so
    // that relative URL references will work.
    ResponseHeaders* response_headers = fetch->response_headers();
    response_headers->SetStatusAndReason(HttpStatus::kMovedPermanently);
    GoogleString admin_with_slash = StrCat(stripped_gurl.AllExceptQuery(), "/");
    response_headers->Add(HttpAttributes::kLocation, admin_with_slash);
    response_headers->Add(HttpAttributes::kContentType, "text/html");
    GoogleString escaped_url;
    HtmlKeywords::Escape(admin_with_slash, &escaped_url);
    fetch->Write(StrCat("Redirecting to URL ", escaped_url), message_handler_);
    fetch->Done(true);
  } else {
    StringPiece leaf = stripped_gurl.LeafSansQuery();
    if ((leaf == "statistics") || (leaf.empty())) {
      StatisticsHandler(*options, kPageSpeedAdmin, fetch, stats);
    } else if (leaf == "graphs") {
      GraphsHandler(*options, kPageSpeedAdmin, fetch, stats);
    } else if (leaf == "config") {
      PrintNormalConfig(kPageSpeedAdmin, fetch, global_system_rewrite_options);
    } else if (leaf == "spdy_config") {
      PrintSpdyConfig(kPageSpeedAdmin, fetch, spdy_config);
    } else if (leaf == "console") {
      // TODO(jmarantz): add vhost-local and aggregate message buffers.
      ConsoleHandler(*global_system_rewrite_options, *options, kPageSpeedAdmin,
                     query_params, fetch, statistics);
    } else if (leaf == "message_history") {
      MessageHistoryHandler(*options, kPageSpeedAdmin, fetch);
    } else if (leaf == "cache") {
      PrintCaches(is_global, kPageSpeedAdmin, stripped_gurl, query_params,
                  options, cache_path, fetch, system_caches,
                  filesystem_metadata_cache, http_cache, metadata_cache,
                  page_property_cache, server_context);
    } else if (leaf == "histograms") {
      PrintHistograms(kPageSpeedAdmin, fetch, stats);
    } else {
      fetch->response_headers()->SetStatusAndReason(HttpStatus::kNotFound);
      fetch->response_headers()->Add(HttpAttributes::kContentType, "text/html");
      fetch->Write("Unknown admin page: ", message_handler_);
      HtmlKeywords::WritePre(leaf, "", fetch, message_handler_);

      // It's possible that the handler is installed on /a/b/c, and we
      // are now reporting "unknown admin page: c".  This is kind of a guess,
      // but provide a nice link here to what might be the correct admin page.
      //
      // This is just a guess, so we don't want to redirect.
      fetch->Write("<br/>Did you mean to visit: ", message_handler_);
      GoogleString escaped_url;
      HtmlKeywords::Escape(StrCat(stripped_gurl.AllExceptQuery(), "/"),
                           &escaped_url);
      fetch->Write(StrCat("<a href='", escaped_url, "'>", escaped_url,
                          "</a>\n"),
                   message_handler_);
      fetch->Done(true);
    }
  }
}

void AdminSite::StatisticsPage(
    bool is_global, const QueryParams& query_params,
    const RewriteOptions* options, AsyncFetch* fetch,
    SystemCaches* system_caches, CacheInterface* filesystem_metadata_cache,
    HTTPCache* http_cache, CacheInterface* metadata_cache,
    PropertyCache* page_property_cache, ServerContext* server_context,
    Statistics* statistics, Statistics* stats,
    SystemRewriteOptions* global_system_rewrite_options,
    const SystemRewriteOptions* spdy_config) {
  if (query_params.Has("json")) {
    ConsoleJsonHandler(query_params, fetch, statistics);
  } else if (query_params.Has("config")) {
    PrintNormalConfig(kStatistics, fetch, global_system_rewrite_options);
  } else if (query_params.Has("spdy_config")) {
    PrintSpdyConfig(kStatistics, fetch, spdy_config);
  } else if (query_params.Has("histograms")) {
    PrintHistograms(kStatistics, fetch, stats);
  } else if (query_params.Has("graphs")) {
    GraphsHandler(*options, kStatistics, fetch, stats);
  } else if (query_params.Has("cache")) {
    GoogleUrl empty_url;
    PrintCaches(is_global, kStatistics, empty_url, query_params,
                options, NULL,  // cache_path is reference from statistics page.
                fetch, system_caches, filesystem_metadata_cache,
                http_cache, metadata_cache, page_property_cache,
                server_context);
  } else {
    StatisticsHandler(*options, kStatistics, fetch, stats);
  }
}

namespace {

// Provides a Done(bool, StringPiece) entry point for use as a Purge
// callback. Translates the success into an Http status code for the
// AsyncFetch, sending any failure reason in the response body.
class PurgeFetchCallbackGasket {
 public:
  PurgeFetchCallbackGasket(AsyncFetch* fetch, MessageHandler* handler)
      : fetch_(fetch),
        message_handler_(handler) {
  }
  void Done(bool success, StringPiece reason) {
    ResponseHeaders* headers = fetch_->response_headers();
    headers->set_status_code(success ? HttpStatus::kOK : HttpStatus::kNotFound);
    headers->Add(HttpAttributes::kContentType, "text/html");
    // TODO(xqyin): Currently we may still return 'purge successful' even if
    // the URL does not exist in our cache. Figure out how to solve this case
    // while we don't want to search the whole cache which could be very large.
    if (success) {
      fetch_->Write("Purge successful\n", message_handler_);
    } else {
      GoogleString buf;
      fetch_->Write(HtmlKeywords::Escape(reason, &buf), message_handler_);
      fetch_->Write("\n", message_handler_);
      fetch_->Write(HtmlKeywords::Escape(error_, &buf), message_handler_);
    }
    fetch_->Write(kBackToPurgeCacheButton, message_handler_);
    fetch_->Done(true);
    delete this;
  }

  void set_error(StringPiece x) { x.CopyToString(&error_); }

 private:
  AsyncFetch* fetch_;
  MessageHandler* message_handler_;
  GoogleString error_;

  DISALLOW_COPY_AND_ASSIGN(PurgeFetchCallbackGasket);
};

}  // namespace

void AdminSite::PurgeHandler(StringPiece url, SystemCachePath* cache_path,
                             AsyncFetch* fetch) {
  PurgeContext* purge_context = cache_path->purge_context();
  int64 now_ms = timer_->NowMs();
  PurgeFetchCallbackGasket* gasket =
      new PurgeFetchCallbackGasket(fetch, message_handler_);
  PurgeContext::PurgeCallback* callback = NewCallback(
      gasket, &PurgeFetchCallbackGasket::Done);
  if (url.ends_with("*")) {
    // If the url is "*" we'll just purge everything.  Note that we will
    // ignore any sub-paths in the expression.  We can only purge the
    // entire cache, or specific URLs, not general wildcards.
    purge_context->SetCachePurgeGlobalTimestampMs(now_ms, callback);
  } else {
    purge_context->AddPurgeUrl(url, now_ms, callback);
  }
}

}  // namespace net_instaweb