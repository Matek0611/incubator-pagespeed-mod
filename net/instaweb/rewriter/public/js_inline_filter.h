/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef NET_INSTAWEB_REWRITER_PUBLIC_JS_INLINE_FILTER_H_
#define NET_INSTAWEB_REWRITER_PUBLIC_JS_INLINE_FILTER_H_

#include <cstddef>

#include "net/instaweb/rewriter/public/common_filter.h"
#include "net/instaweb/rewriter/public/resource.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "net/instaweb/rewriter/public/rewrite_options.h"
#include "net/instaweb/rewriter/public/script_tag_scanner.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_filter.h"
#include "pagespeed/kernel/html/html_node.h"
#include "pagespeed/kernel/http/semantic_type.h"

namespace net_instaweb {
class Statistics;
class Variable;

// Inline small Javascript files.
class JsInlineFilter : public CommonFilter {
 public:
  static const char kNumJsInlined[];
  explicit JsInlineFilter(RewriteDriver* driver);
  ~JsInlineFilter() override;

  void StartDocumentImpl() override;
  void EndDocument() override;
  void StartElementImpl(HtmlElement* element) override;
  void EndElementImpl(HtmlElement* element) override;
  void Characters(HtmlCharactersNode* characters) override;
  const char* Name() const override { return "InlineJs"; }
  // Inlining javascript from unauthorized domains into HTML is considered
  // safe because it does not cause any new content to be executed compared
  // to the unoptimized page.
  RewriteDriver::InlineAuthorizationPolicy AllowUnauthorizedDomain()
      const override {
    return driver()->options()->HasInlineUnauthorizedResourceType(
               semantic_type::kScript)
               ? RewriteDriver::kInlineUnauthorizedResources
               : RewriteDriver::kInlineOnlyAuthorizedResources;
  }
  bool IntendedForInlining() const override { return true; }
  ScriptUsage GetScriptUsage() const override { return kWillInjectScripts; }

  static void InitStats(Statistics* statistics);

 private:
  class Context;
  friend class Context;

  bool ShouldInline(const ResourcePtr& resource, GoogleString* reason) const;
  void RenderInline(const ResourcePtr& resource, const StringPiece& text,
                    HtmlElement* element);

  const size_t size_threshold_bytes_;
  ScriptTagScanner script_tag_scanner_;

  // This is set to true during StartElement() for a <script> tag that we
  // should maybe inline, but may be set back to false by Characters().  If it
  // is still true when we hit the corresponding EndElement(), then we'll
  // inline the script (and set it back to false).  It should never be true
  // outside of <script> and </script>.
  bool should_inline_;

  Variable* num_js_inlined_;

  DISALLOW_COPY_AND_ASSIGN(JsInlineFilter);
};

}  // namespace net_instaweb

#endif  // NET_INSTAWEB_REWRITER_PUBLIC_JS_INLINE_FILTER_H_
