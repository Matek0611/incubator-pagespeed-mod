/*
 * Copyright 2014 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Author: stevensr@google.com (Ryan Stevens)

#include "net/instaweb/rewriter/public/mobilize_rewrite_filter.h"

#include <algorithm>

#include "base/logging.h"
#include "net/instaweb/rewriter/public/domain_lawyer.h"
#include "net/instaweb/rewriter/public/rewrite_driver.h"
#include "pagespeed/kernel/base/basictypes.h"
#include "pagespeed/kernel/base/statistics.h"
#include "pagespeed/kernel/base/string.h"
#include "pagespeed/kernel/base/string_util.h"
#include "pagespeed/kernel/html/html_element.h"
#include "pagespeed/kernel/html/html_node.h"

namespace net_instaweb {

extern const char* CSS_mobilize_css;

const MobileRole MobileRole::kMobileRoles[MobileRole::kInvalid] = {
  // This is the order that the HTML content will be rearranged.
  MobileRole(MobileRole::kKeeper, "keeper"),
  MobileRole(MobileRole::kHeader, "header"),
  MobileRole(MobileRole::kNavigational, "navigational"),
  MobileRole(MobileRole::kContent, "content"),
  MobileRole(MobileRole::kMarginal, "marginal")
};

const char MobilizeRewriteFilter::kPagesMobilized[] =
    "mobilization_pages_rewritten";
const char MobilizeRewriteFilter::kKeeperBlocks[] =
    "mobilization_keeper_blocks_moved";
const char MobilizeRewriteFilter::kHeaderBlocks[] =
    "mobilization_header_blocks_moved";
const char MobilizeRewriteFilter::kNavigationalBlocks[] =
    "mobilization_navigational_blocks_moved";
const char MobilizeRewriteFilter::kContentBlocks[] =
    "mobilization_content_blocks_moved";
const char MobilizeRewriteFilter::kMarginalBlocks[] =
    "mobilization_marginal_blocks_moved";
const char MobilizeRewriteFilter::kDeletedElements[] =
    "mobilization_elements_deleted";

namespace {

// The 'book' says to use add ",user-scalable=no" but jmarantz hates
// this.  I want to be able to zoom in.  Debate with the writers of
// that book will need to occur.
const char kViewportContent[] = "width=device-width";

const HtmlName::Keyword kPreserveNavTags[] = {HtmlName::kA};
const HtmlName::Keyword kTableTags[] = {
  HtmlName::kCaption, HtmlName::kCol, HtmlName::kColgroup, HtmlName::kTable,
  HtmlName::kTbody, HtmlName::kTd, HtmlName::kTfoot, HtmlName::kTh,
  HtmlName::kThead, HtmlName::kTr};
const HtmlName::Keyword kTableTagsToBr[] = {HtmlName::kTable, HtmlName::kTr};

const char* const kPolymerElementLinks[] = {
    "core-drawer-panel/core-drawer-panel.html",
    "core-header-panel/core-header-panel.html",
    "core-icon-button/core-icon-button.html",
    "core-icons/core-icons.html",
    "core-item/core-item.html",
    "core-menu/core-menu.html",
    "core-menu/core-submenu.html",
    "core-scaffold/core-scaffold.html",
    "core-toolbar/core-toolbar.html",
    "paper-icon-button/paper-icon-button.html",
    "paper-fab/paper-fab.html"};

const char* const kPolymerCustomElementLinks[] = {"polymer-elements.html"};

#ifndef NDEBUG
void CheckKeywordsSorted(const HtmlName::Keyword* list, int len) {
  for (int i = 1; i < len; ++i) {
    DCHECK(list[i - 1] < list[i]);
  }
}
#endif  // #ifndef NDEBUG
}  // namespace

const HtmlName::Keyword MobilizeRewriteFilter::kKeeperTags[] = {
  HtmlName::kArea, HtmlName::kMap, HtmlName::kScript, HtmlName::kStyle};
const int MobilizeRewriteFilter::kNumKeeperTags = arraysize(kKeeperTags);

MobilizeRewriteFilter::MobilizeRewriteFilter(RewriteDriver* rewrite_driver)
    : driver_(rewrite_driver),
      body_element_depth_(0),
      nav_element_depth_(0),
      reached_reorder_containers_(false),
      found_viewport_(false),
      added_style_(false),
      added_containers_(false),
      added_mob_js_(false),
      in_script_(false),
      use_cxx_layout_(false),
      use_js_layout_(rewrite_driver->options()->mob_layout()),
      use_js_logo_(rewrite_driver->options()->mob_logo()),
      use_js_nav_(rewrite_driver->options()->mob_nav()),
      style_css_(CSS_mobilize_css) {

  // If a domain proxy-suffix is specified, and it starts with ".",
  // then we'll remove the "." from that and use that as the location
  // of the shared static files (JS, CSS, and Polymer HTML).  E.g.
  // for a proxy_suffix of ".suffix" we'll look for static files in
  // "//suffix/static/".
  StringPiece suffix(
      rewrite_driver->options()->domain_lawyer()->proxy_suffix());
  if (!suffix.empty() && suffix.starts_with(".")) {
    suffix.remove_prefix(1);
    static_file_prefix_ = StrCat("//", suffix, "/static/");
  }

  use_cxx_layout_ = !(use_js_layout_ || use_js_logo_ || use_js_nav_);
  Statistics* stats = rewrite_driver->statistics();
  num_pages_mobilized_ = stats->GetVariable(kPagesMobilized);
  num_keeper_blocks_ = stats->GetVariable(kKeeperBlocks);
  num_header_blocks_ = stats->GetVariable(kHeaderBlocks);
  num_navigational_blocks_ = stats->GetVariable(kNavigationalBlocks);
  num_content_blocks_ = stats->GetVariable(kContentBlocks);
  num_marginal_blocks_ = stats->GetVariable(kMarginalBlocks);
  num_elements_deleted_ = stats->GetVariable(kDeletedElements);
#ifndef NDEBUG
  CheckKeywordsSorted(kKeeperTags, kNumKeeperTags);
  CheckKeywordsSorted(kPreserveNavTags, arraysize(kPreserveNavTags));
  CheckKeywordsSorted(kTableTags, arraysize(kTableTags));
  CheckKeywordsSorted(kTableTagsToBr, arraysize(kTableTagsToBr));
#endif  // #ifndef NDEBUG
}

MobilizeRewriteFilter::~MobilizeRewriteFilter() {}

void MobilizeRewriteFilter::InitStats(Statistics* statistics) {
  statistics->AddVariable(kPagesMobilized);
  statistics->AddVariable(kKeeperBlocks);
  statistics->AddVariable(kHeaderBlocks);
  statistics->AddVariable(kNavigationalBlocks);
  statistics->AddVariable(kContentBlocks);
  statistics->AddVariable(kMarginalBlocks);
  statistics->AddVariable(kDeletedElements);
}

void MobilizeRewriteFilter::StartDocument() {
  body_element_depth_ = 0;
  nav_element_depth_ = 0;
  reached_reorder_containers_ = false;
  found_viewport_ = false;
  added_style_ = false;
  added_containers_ = false;
  added_mob_js_ = false;
  in_script_ = false;
  element_roles_stack_.clear();
  nav_keyword_stack_.clear();
}

void MobilizeRewriteFilter::EndDocument() {
  num_pages_mobilized_->Add(1);
}

void MobilizeRewriteFilter::StartElement(HtmlElement* element) {
  HtmlName::Keyword keyword = element->keyword();

  // Unminify jquery for javascript debugging.
  if ((keyword == HtmlName::kScript) && !use_cxx_layout_) {
    in_script_ = true;

    HtmlElement::Attribute* src_attribute =
        element->FindAttribute(HtmlName::kSrc);
    if (src_attribute != NULL) {
      StringPiece src(src_attribute->DecodedValueOrNull());
      if (src.find("jquery.min.js") != StringPiece::npos) {
        GoogleString new_value = src.as_string();
        GlobalReplaceSubstring("/jquery.min.js", "/jquery.js", &new_value);
        src_attribute->SetValue(new_value);
      }
    }
  }

  // Remove any existing viewport tags, other than the one we created
  // at start of head.
  if (keyword == HtmlName::kMeta) {
    HtmlElement::Attribute* name_attribute =
        element->FindAttribute(HtmlName::kName);
    if (name_attribute != NULL &&
        (StringPiece(name_attribute->escaped_value()) == "viewport")) {
      StringPiece content(element->AttributeValue(HtmlName::kContent));
      if (content == kViewportContent) {
        found_viewport_ = true;
      } else {
        driver_->DeleteNode(element);
        num_elements_deleted_->Add(1);
      }
      return;
    }
  }

  if (keyword == HtmlName::kBody) {
    // TODO(jmarantz): Prevents FOUC for polymer but we have all other kinds
    // of FOUC anyway.  Resolve this when we have resolved those.
    // driver_->AddAttribute(element, "unresolved", "");

    ++body_element_depth_;
    if (use_cxx_layout_) {
      AddReorderContainers(element);
    }
  } else if (body_element_depth_ > 0) {
    if (use_cxx_layout_) {
      HandleStartTagInBody(element);
    }
  }
}

void MobilizeRewriteFilter::EndElement(HtmlElement* element) {
  HtmlName::Keyword keyword = element->keyword();

  if (keyword == HtmlName::kScript) {
    in_script_ = false;
  }

  if (keyword == HtmlName::kBody) {
    --body_element_depth_;
    if (body_element_depth_ == 0) {
      if (use_js_layout_ || use_js_nav_) {
        if (!added_mob_js_) {
          added_mob_js_ = true;

          // TODO(jmarantz): Consider using CommonFilter::InsertNodeAtBodyEnd.
          if (use_js_layout_) {
            HtmlElement* script = driver_->NewElement(element->parent(),
                                                      HtmlName::kScript);
            script->set_style(HtmlElement::EXPLICIT_CLOSE);
            driver_->InsertNodeAfterCurrent(script);
            driver_->AddAttribute(script, HtmlName::kSrc,
                                  StrCat(static_file_prefix_, "mob.js"));
          }
          if (use_js_nav_) {
            HtmlElement* script =
                driver_->NewElement(element->parent(), HtmlName::kScript);
            script->set_style(HtmlElement::EXPLICIT_CLOSE);
            driver_->InsertNodeAfterCurrent(script);
            driver_->AddAttribute(script, HtmlName::kSrc,
                                  StrCat(static_file_prefix_, "mob_nav.js"));
          }
          if (use_js_logo_) {
            HtmlElement* script =
                driver_->NewElement(element->parent(), HtmlName::kScript);
            script->set_style(HtmlElement::EXPLICIT_CLOSE);
            driver_->InsertNodeAfterCurrent(script);
            driver_->AddAttribute(script, HtmlName::kSrc,
                                  StrCat(static_file_prefix_, "mob_logo.js"));
          }
        }
      } else {
        RemoveReorderContainers();
      }
      reached_reorder_containers_ = false;
    }
  } else if (body_element_depth_ == 0 && keyword == HtmlName::kHead) {
    // TODO(jmarantz): this uses AppendChild, but probably should use
    // InsertBeforeCurrent to make it work with flush windows.
    AddStyleAndViewport(element);

    // TODO(jmarantz): if we want to debug with Closure constructs, uncomment:
    // HtmlElement* script_element =
    //     driver_->NewElement(element, HtmlName::kScript);
    // driver_->AppendChild(element, script_element);
    // driver_->AddAttribute(script_element, HtmlName::kSrc,
    //                       StrCat(static_file_prefix_, "closure/base.js"));
  } else if (body_element_depth_ > 0) {
    if (use_cxx_layout_) {
      HandleEndTagInBody(element);
    }
  }
}

void MobilizeRewriteFilter::Characters(HtmlCharactersNode* characters) {
  if (!use_cxx_layout_) {
    if (in_script_) {
      // This is a temporary hack for removing a SPOF from
      // http://www.cardpersonalizzate.it/, whose reference
      // to a file in e.mouseflow.com hangs and stops the
      // browser from making progress.
      GoogleString* contents = characters->mutable_contents();
      if (contents->find("//e.mouseflow.com/projects") != GoogleString::npos) {
        *contents = StrCat("/*", *contents, "*/");
      }
    }
    return;
  }
  if (body_element_depth_ == 0 || reached_reorder_containers_) {
    return;
  }

  bool del = false;
  GoogleString debug_msg;
  if (!InImportantElement()) {
    del = true;
    debug_msg = "Deleted characters which were not in an element which"
        " was tagged as important: ";
  } else if (nav_element_depth_ > 0 && nav_keyword_stack_.empty()) {
    del = true;
    debug_msg = "Deleted characters inside a navigational section"
        " which were not considered to be relevant to navigation: ";
  }

  if (del) {
    if (driver_->DebugMode() && !OnlyWhitespace(characters->contents())) {
      GoogleString msg = debug_msg + characters->contents();
      driver_->InsertDebugComment(msg, characters);
    }
    driver_->DeleteNode(characters);
    num_elements_deleted_->Add(1);
  }
}

void MobilizeRewriteFilter::HandleStartTagInBody(HtmlElement* element) {
  HtmlName::Keyword keyword = element->keyword();
  if (reached_reorder_containers_) {
    // Stop rewriting once we've reached the containers at the end of the body.
  } else if (IsReorderContainer(element)) {
    reached_reorder_containers_ = true;
  } else if (CheckForKeyword(kTableTags, arraysize(kTableTags), keyword)) {
    // Remove any table tags.
    if (CheckForKeyword(kTableTagsToBr, arraysize(kTableTagsToBr), keyword)) {
      HtmlElement* added_br_element = driver_->NewElement(
          element->parent(), HtmlName::kBr);
      added_br_element->set_style(HtmlElement::IMPLICIT_CLOSE);
      driver_->InsertElementAfterElement(element, added_br_element);
    }
    if (driver_->DebugMode()) {
      GoogleString msg(StrCat("Deleted table tag: ", element->name_str()));
      driver_->InsertDebugComment(msg, element);
    }
    driver_->DeleteSavingChildren(element);
    num_elements_deleted_->Add(1);
  } else if (GetMobileRole(element) != MobileRole::kInvalid) {
    MobileRole::Level element_role = GetMobileRole(element);
    // Record that we are starting an element with a mobile role attribute.
    element_roles_stack_.push_back(element_role);
    if (element_role == MobileRole::kNavigational) {
      ++nav_element_depth_;
      if (nav_element_depth_ == 1) {
        nav_keyword_stack_.clear();
      }
    }
  } else if (nav_element_depth_ > 0) {
    // Remove all navigational content not inside a desired tag.
    if (CheckForKeyword(
            kPreserveNavTags, arraysize(kPreserveNavTags), keyword)) {
      nav_keyword_stack_.push_back(keyword);
    }
    if (nav_keyword_stack_.empty()) {
      if (driver_->DebugMode()) {
        GoogleString msg(
            StrCat("Deleted non-nav element in navigational section: ",
                   element->name_str()));
        driver_->InsertDebugComment(msg, element);
      }
      driver_->DeleteSavingChildren(element);
      num_elements_deleted_->Add(1);
    }
  } else if (!InImportantElement()) {
    if (driver_->DebugMode()) {
      GoogleString msg(
          StrCat("Deleted element which did not have a mobile role: ",
                 element->name_str()));
      driver_->InsertDebugComment(msg, element);
    }
    driver_->DeleteSavingChildren(element);
    num_elements_deleted_->Add(1);
  }
}

void MobilizeRewriteFilter::HandleEndTagInBody(HtmlElement* element) {
  if (reached_reorder_containers_) {
    // Stop rewriting once we've reached the containers at the end of the body.
  } else if (GetMobileRole(element) != MobileRole::kInvalid) {
    MobileRole::Level element_role = GetMobileRole(element);
    element_roles_stack_.pop_back();
    if (element_role == MobileRole::kNavigational) {
      --nav_element_depth_;
    }
    // Record that we've left an element with a mobile role attribute. If we are
    // no longer in one, we can move all the content of this element into its
    // appropriate container for reordering.
    HtmlElement* mobile_role_container =
        MobileRoleToContainer(element_role);
    DCHECK(mobile_role_container != NULL)
        << "Reorder containers were never initialized.";
    // Move element and its children into its container, unless we are already
    // in an element that has the same mobile role.
    if (element_roles_stack_.empty() ||
        element_roles_stack_.back() != element_role) {
      driver_->MoveCurrentInto(mobile_role_container);
      LogMovedBlock(element_role);
    }
  } else if (nav_element_depth_ > 0) {
    HtmlName::Keyword keyword = element->keyword();
    if (!nav_keyword_stack_.empty() && (keyword == nav_keyword_stack_.back())) {
      nav_keyword_stack_.pop_back();
    }
  }
}

void MobilizeRewriteFilter::AddStyleAndViewport(HtmlElement* element) {
  if (!added_style_) {
    added_style_ = true;

    if (use_cxx_layout_) {
      HtmlElement* added_style_element = driver_->NewElement(
          element, HtmlName::kStyle);
      driver_->AppendChild(element, added_style_element);
      HtmlCharactersNode* add_style_text = driver_->NewCharactersNode(
          added_style_element, style_css_);
      driver_->AppendChild(added_style_element, add_style_text);
    }

    // <meta name="viewport"... />
    if (!found_viewport_) {
      found_viewport_ = true;
      HtmlElement* added_viewport_element = driver_->NewElement(
          element, HtmlName::kMeta);
      added_viewport_element->set_style(HtmlElement::BRIEF_CLOSE);
      added_viewport_element->AddAttribute(
          driver_->MakeName(HtmlName::kName), "viewport",
          HtmlElement::SINGLE_QUOTE);
      added_viewport_element->AddAttribute(
          driver_->MakeName(HtmlName::kContent), kViewportContent,
          HtmlElement::SINGLE_QUOTE);
      driver_->AppendChild(element, added_viewport_element);
    }

    // <style>...</style>
    if (!use_cxx_layout_) {
      HtmlElement* link = driver_->NewElement(element, HtmlName::kLink);
      driver_->AppendChild(element, link);
      driver_->AddAttribute(link, HtmlName::kRel, "stylesheet");
      driver_->AddAttribute(link, HtmlName::kHref, StrCat(static_file_prefix_,
                                                          "lite.css"));
    }

    if (use_js_nav_) {
      GoogleString polymer_base_url = StrCat(static_file_prefix_, "polymer/");

      // Insert the script tag for polymer's platform.js.
      HtmlElement* polymer_script =
          driver_->NewElement(element, HtmlName::kScript);
      driver_->AppendChild(element, polymer_script);
      polymer_script->AddAttribute(
          driver_->MakeName(HtmlName::kSrc),
          StrCat(polymer_base_url, "platform/platform.js"),
          HtmlElement::DOUBLE_QUOTE);
      polymer_script->set_style(HtmlElement::EXPLICIT_CLOSE);

      // Insert the link tags for the polymer elements
      for (int i = 0, n = arraysize(kPolymerElementLinks); i < n; ++i) {
        InsertPolymerLink(element,
                          StrCat(polymer_base_url, kPolymerElementLinks[i]));
      }

      for (int i = 0, n = arraysize(kPolymerCustomElementLinks); i < n; ++i) {
        InsertPolymerLink(
            element, StrCat(static_file_prefix_,
                            kPolymerCustomElementLinks[i]));
      }
    }
  }
}

void MobilizeRewriteFilter::InsertPolymerLink(HtmlElement* element,
                                              StringPiece url) {
  HtmlElement* polymer_link = driver_->NewElement(element, HtmlName::kLink);
  driver_->AppendChild(element, polymer_link);
  polymer_link->AddAttribute(driver_->MakeName(HtmlName::kRel), "import",
                             HtmlElement::DOUBLE_QUOTE);
  polymer_link->AddAttribute(driver_->MakeName(HtmlName::kHref), url,
                             HtmlElement::DOUBLE_QUOTE);
}

// Adds containers at the end of the element (preferrably the body), which we
// use to reorganize elements in the DOM by moving elements into the correct
// container. Later, we will delete these elements once the HTML has been
// restructured.
void MobilizeRewriteFilter::AddReorderContainers(HtmlElement* element) {
  if (!added_containers_) {
    mobile_role_containers_.clear();
    for (int i = 0; i < MobileRole::kInvalid; ++i) {
      MobileRole::Level level = static_cast<MobileRole::Level>(i);
      HtmlElement* added_container = driver_->NewElement(
          element, HtmlName::kDiv);
      added_container->AddAttribute(
          driver_->MakeName(HtmlName::kName),
          MobileRole::StringFromLevel(level),
          HtmlElement::SINGLE_QUOTE);
      driver_->AppendChild(element, added_container);
      mobile_role_containers_.push_back(added_container);
    }
    added_containers_ = true;
  }
}

void MobilizeRewriteFilter::RemoveReorderContainers() {
  if (added_containers_) {
    for (int i = 0, n = mobile_role_containers_.size(); i < n; ++i) {
      if (driver_->DebugMode()) {
        MobileRole::Level level = static_cast<MobileRole::Level>(i);
        GoogleString msg(StrCat("End section: ",
                                MobileRole::StringFromLevel(level)));
        driver_->InsertDebugComment(msg, mobile_role_containers_[i]);
      }
      driver_->DeleteSavingChildren(mobile_role_containers_[i]);
    }
    mobile_role_containers_.clear();
    added_containers_ = false;
  }
}

bool MobilizeRewriteFilter::IsReorderContainer(HtmlElement* element) {
  for (int i = 0, n = mobile_role_containers_.size(); i < n; ++i) {
    if (element == mobile_role_containers_[i]) {
      return true;
    }
  }
  return false;
}

// Maps each mobile role to the container we created for it, or NULL for
// unrecognized mobile roles.
HtmlElement* MobilizeRewriteFilter::MobileRoleToContainer(
    MobileRole::Level level) {
  return (level == MobileRole::kInvalid) ?
      NULL : mobile_role_containers_[level];
}

const MobileRole* MobileRole::FromString(const StringPiece& mobile_role) {
  for (int i = 0, n = arraysize(kMobileRoles); i < n; ++i) {
    if (mobile_role == kMobileRoles[i].value) {
      return &kMobileRoles[i];
    }
  }
  return NULL;
}

MobileRole::Level MobileRole::LevelFromString(const StringPiece& mobile_role) {
  const MobileRole* role = FromString(mobile_role);
  if (role == NULL) {
    return kInvalid;
  } else {
    return role->level;
  }
}

MobileRole::Level MobilizeRewriteFilter::GetMobileRole(
    HtmlElement* element) {
  HtmlElement::Attribute* mobile_role_attribute =
      element->FindAttribute(HtmlName::kDataMobileRole);
  if (mobile_role_attribute) {
    return MobileRole::LevelFromString(mobile_role_attribute->escaped_value());
  } else {
    if (CheckForKeyword(kKeeperTags, kNumKeeperTags,
                        element->keyword())) {
      return MobileRole::kKeeper;
    }
    return MobileRole::kInvalid;
  }
}

bool MobilizeRewriteFilter::CheckForKeyword(
    const HtmlName::Keyword* sorted_list, int len, HtmlName::Keyword keyword) {
  return std::binary_search(sorted_list, sorted_list+len, keyword);
}

void MobilizeRewriteFilter::LogMovedBlock(MobileRole::Level level) {
  switch (level) {
    case MobileRole::kKeeper:
      num_keeper_blocks_->Add(1);
      break;
    case MobileRole::kHeader:
      num_header_blocks_->Add(1);
      break;
    case MobileRole::kNavigational:
      num_navigational_blocks_->Add(1);
      break;
    case MobileRole::kContent:
      num_content_blocks_->Add(1);
      break;
    case MobileRole::kMarginal:
      num_marginal_blocks_->Add(1);
      break;
    case MobileRole::kInvalid:
      // Should not happen.
      LOG(DFATAL) << "Attepted to move kInvalid element";
      break;
  }
}

}  // namespace net_instaweb
