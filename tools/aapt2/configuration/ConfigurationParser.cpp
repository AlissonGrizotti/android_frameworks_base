/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "configuration/ConfigurationParser.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <utility>

#include "android-base/file.h"
#include "android-base/logging.h"

#include "ConfigDescription.h"
#include "Diagnostics.h"
#include "ResourceUtils.h"
#include "configuration/ConfigurationParser.internal.h"
#include "io/File.h"
#include "io/FileSystem.h"
#include "io/StringStream.h"
#include "util/Files.h"
#include "util/Maybe.h"
#include "util/Util.h"
#include "xml/XmlActionExecutor.h"
#include "xml/XmlDom.h"
#include "xml/XmlUtil.h"

namespace aapt {

namespace {

using ::aapt::configuration::Abi;
using ::aapt::configuration::AndroidManifest;
using ::aapt::configuration::AndroidSdk;
using ::aapt::configuration::ConfiguredArtifact;
using ::aapt::configuration::DeviceFeature;
using ::aapt::configuration::Entry;
using ::aapt::configuration::GlTexture;
using ::aapt::configuration::Group;
using ::aapt::configuration::Locale;
using ::aapt::configuration::OutputArtifact;
using ::aapt::configuration::PostProcessingConfiguration;
using ::aapt::configuration::handler::AbiGroupTagHandler;
using ::aapt::configuration::handler::AndroidSdkGroupTagHandler;
using ::aapt::configuration::handler::ArtifactFormatTagHandler;
using ::aapt::configuration::handler::ArtifactTagHandler;
using ::aapt::configuration::handler::DeviceFeatureGroupTagHandler;
using ::aapt::configuration::handler::GlTextureGroupTagHandler;
using ::aapt::configuration::handler::LocaleGroupTagHandler;
using ::aapt::configuration::handler::ScreenDensityGroupTagHandler;
using ::aapt::io::IFile;
using ::aapt::io::RegularFile;
using ::aapt::io::StringInputStream;
using ::aapt::util::TrimWhitespace;
using ::aapt::xml::Element;
using ::aapt::xml::NodeCast;
using ::aapt::xml::XmlActionExecutor;
using ::aapt::xml::XmlActionExecutorPolicy;
using ::aapt::xml::XmlNodeAction;
using ::android::StringPiece;
using ::android::base::ReadFileToString;

const std::unordered_map<StringPiece, Abi> kStringToAbiMap = {
    {"armeabi", Abi::kArmeV6}, {"armeabi-v7a", Abi::kArmV7a},  {"arm64-v8a", Abi::kArm64V8a},
    {"x86", Abi::kX86},        {"x86_64", Abi::kX86_64},       {"mips", Abi::kMips},
    {"mips64", Abi::kMips64},  {"universal", Abi::kUniversal},
};
const std::array<StringPiece, 8> kAbiToStringMap = {
    {"armeabi", "armeabi-v7a", "arm64-v8a", "x86", "x86_64", "mips", "mips64", "universal"}};

constexpr const char* kAaptXmlNs = "http://schemas.android.com/tools/aapt";

/** A default noop diagnostics context. */
class NoopDiagnostics : public IDiagnostics {
 public:
  void Log(Level level, DiagMessageActual& actualMsg) override {}
};
NoopDiagnostics noop_;

std::string GetLabel(const Element* element, IDiagnostics* diag) {
  std::string label;
  for (const auto& attr : element->attributes) {
    if (attr.name == "label") {
      label = attr.value;
      break;
    }
  }

  if (label.empty()) {
    diag->Error(DiagMessage() << "No label found for element " << element->name);
  }
  return label;
}

/** XML node visitor that removes all of the namespace URIs from the node and all children. */
class NamespaceVisitor : public xml::Visitor {
 public:
  void Visit(xml::Element* node) override {
    node->namespace_uri.clear();
    VisitChildren(node);
  }
};

/** Copies the values referenced in a configuration group to the target list. */
template <typename T>
bool CopyXmlReferences(const Maybe<std::string>& name, const Group<T>& groups,
                       std::vector<T>* target) {
  // If there was no item configured, there is nothing to do and no error.
  if (!name) {
    return true;
  }

  // If the group could not be found, then something is wrong.
  auto group = groups.find(name.value());
  if (group == groups.end()) {
    return false;
  }

  for (const T& item : group->second) {
    target->push_back(item);
  }
  return true;
}

/**
 * Attempts to replace the placeholder in the name string with the provided value. Returns true on
 * success, or false if the either the placeholder is not found in the name, or the value is not
 * present and the placeholder was.
 */
bool ReplacePlaceholder(const StringPiece& placeholder, const Maybe<StringPiece>& value,
                        std::string* name, IDiagnostics* diag) {
  size_t offset = name->find(placeholder.data());
  bool found = (offset != std::string::npos);

  // Make sure the placeholder was present if the desired value is present.
  if (!found) {
    if (value) {
      diag->Error(DiagMessage() << "Missing placeholder for artifact: " << placeholder);
      return false;
    }
    return true;
  }

  DCHECK(found) << "Missing return path for placeholder not found";

  // Make sure the placeholder was not present if the desired value was not present.
  if (!value) {
    diag->Error(DiagMessage() << "Placeholder present but no value for artifact: " << placeholder);
    return false;
  }

  name->replace(offset, placeholder.length(), value.value().data());

  // Make sure there was only one instance of the placeholder.
  if (name->find(placeholder.data()) != std::string::npos) {
    diag->Error(DiagMessage() << "Placeholder present multiple times: " << placeholder);
    return false;
  }
  return true;
}

/**
 * An ActionHandler for processing XML elements in the XmlActionExecutor. Returns true if the
 * element was successfully processed, otherwise returns false.
 */
using ActionHandler = std::function<bool(configuration::PostProcessingConfiguration* config,
                                         xml::Element* element, IDiagnostics* diag)>;

/** Binds an ActionHandler to the current configuration being populated. */
xml::XmlNodeAction::ActionFuncWithDiag Bind(configuration::PostProcessingConfiguration* config,
                                            const ActionHandler& handler) {
  return [config, handler](xml::Element* root_element, SourcePathDiagnostics* diag) {
    return handler(config, root_element, diag);
  };
}

/** Returns the binary reprasentation of the XML configuration. */
Maybe<PostProcessingConfiguration> ExtractConfiguration(const std::string& contents,
                                                        IDiagnostics* diag) {
  StringInputStream in(contents);
  std::unique_ptr<xml::XmlResource> doc = xml::Inflate(&in, diag, Source("config.xml"));
  if (!doc) {
    return {};
  }

  // Strip any namespaces from the XML as the XmlActionExecutor ignores anything with a namespace.
  Element* root = doc->root.get();
  if (root == nullptr) {
    diag->Error(DiagMessage() << "Could not find the root element in the XML document");
    return {};
  }

  std::string& xml_ns = root->namespace_uri;
  if (!xml_ns.empty()) {
    if (xml_ns != kAaptXmlNs) {
      diag->Error(DiagMessage() << "Unknown namespace found on root element: " << xml_ns);
      return {};
    }

    xml_ns.clear();
    NamespaceVisitor visitor;
    root->Accept(&visitor);
  }

  XmlActionExecutor executor;
  XmlNodeAction& root_action = executor["post-process"];
  XmlNodeAction& artifacts_action = root_action["artifacts"];
  XmlNodeAction& groups_action = root_action["groups"];

  PostProcessingConfiguration config;

  // Parse the artifact elements.
  artifacts_action["artifact"].Action(Bind(&config, ArtifactTagHandler));
  artifacts_action["artifact-format"].Action(Bind(&config, ArtifactFormatTagHandler));

  // Parse the different configuration groups.
  groups_action["abi-group"].Action(Bind(&config, AbiGroupTagHandler));
  groups_action["screen-density-group"].Action(Bind(&config, ScreenDensityGroupTagHandler));
  groups_action["locale-group"].Action(Bind(&config, LocaleGroupTagHandler));
  groups_action["android-sdk-group"].Action(Bind(&config, AndroidSdkGroupTagHandler));
  groups_action["gl-texture-group"].Action(Bind(&config, GlTextureGroupTagHandler));
  groups_action["device-feature-group"].Action(Bind(&config, DeviceFeatureGroupTagHandler));

  if (!executor.Execute(XmlActionExecutorPolicy::kNone, diag, doc.get())) {
    diag->Error(DiagMessage() << "Could not process XML document");
    return {};
  }

  return {config};
}

/** Converts a ConfiguredArtifact into an OutputArtifact. */
Maybe<OutputArtifact> ToOutputArtifact(const ConfiguredArtifact& artifact,
                                       const std::string& apk_name,
                                       const PostProcessingConfiguration& config,
                                       IDiagnostics* diag) {
  if (!artifact.name && !config.artifact_format) {
    diag->Error(
        DiagMessage() << "Artifact does not have a name and no global name template defined");
    return {};
  }

  Maybe<std::string> artifact_name =
      (artifact.name) ? artifact.Name(apk_name, diag)
                      : artifact.ToArtifactName(config.artifact_format.value(), apk_name, diag);

  if (!artifact_name) {
    diag->Error(DiagMessage() << "Could not determine split APK artifact name");
    return {};
  }

  OutputArtifact output_artifact;
  output_artifact.name = artifact_name.value();

  SourcePathDiagnostics src_diag{{output_artifact.name}, diag};
  bool has_errors = false;

  if (!CopyXmlReferences(artifact.abi_group, config.abi_groups, &output_artifact.abis)) {
    src_diag.Error(DiagMessage() << "Could not lookup required ABIs: "
                                 << artifact.abi_group.value());
    has_errors = true;
  }

  if (!CopyXmlReferences(artifact.locale_group, config.locale_groups, &output_artifact.locales)) {
    src_diag.Error(DiagMessage() << "Could not lookup required locales: "
                                 << artifact.locale_group.value());
    has_errors = true;
  }

  if (!CopyXmlReferences(artifact.screen_density_group, config.screen_density_groups,
                         &output_artifact.screen_densities)) {
    src_diag.Error(DiagMessage() << "Could not lookup required screen densities: "
                                 << artifact.screen_density_group.value());
    has_errors = true;
  }

  if (!CopyXmlReferences(artifact.device_feature_group, config.device_feature_groups,
                         &output_artifact.features)) {
    src_diag.Error(DiagMessage() << "Could not lookup required device features: "
                                 << artifact.device_feature_group.value());
    has_errors = true;
  }

  if (!CopyXmlReferences(artifact.gl_texture_group, config.gl_texture_groups,
                         &output_artifact.textures)) {
    src_diag.Error(DiagMessage() << "Could not lookup required OpenGL texture formats: "
                                 << artifact.gl_texture_group.value());
    has_errors = true;
  }

  if (artifact.android_sdk_group) {
    auto entry = config.android_sdk_groups.find(artifact.android_sdk_group.value());
    if (entry == config.android_sdk_groups.end()) {
      src_diag.Error(DiagMessage() << "Could not lookup required Android SDK version: "
                                   << artifact.android_sdk_group.value());
      has_errors = true;
    } else {
      output_artifact.android_sdk = {entry->second};
    }
  }

  if (has_errors) {
    return {};
  }
  return {output_artifact};
}

}  // namespace

namespace configuration {

const StringPiece& AbiToString(Abi abi) {
  return kAbiToStringMap.at(static_cast<size_t>(abi));
}

/**
 * Returns the common artifact base name from a template string.
 */
Maybe<std::string> ToBaseName(std::string result, const StringPiece& apk_name, IDiagnostics* diag) {
  const StringPiece ext = file::GetExtension(apk_name);
  size_t end_index = apk_name.to_string().rfind(ext.to_string());
  const std::string base_name =
      (end_index != std::string::npos) ? std::string{apk_name.begin(), end_index} : "";

  // Base name is optional.
  if (result.find("${basename}") != std::string::npos) {
    Maybe<StringPiece> maybe_base_name =
        base_name.empty() ? Maybe<StringPiece>{} : Maybe<StringPiece>{base_name};
    if (!ReplacePlaceholder("${basename}", maybe_base_name, &result, diag)) {
      return {};
    }
  }

  // Extension is optional.
  if (result.find("${ext}") != std::string::npos) {
    // Make sure we disregard the '.' in the extension when replacing the placeholder.
    if (!ReplacePlaceholder("${ext}", {ext.substr(1)}, &result, diag)) {
      return {};
    }
  } else {
    // If no extension is specified, and the name template does not end in the current extension,
    // add the existing extension.
    if (!util::EndsWith(result, ext)) {
      result.append(ext.to_string());
    }
  }

  return result;
}

Maybe<std::string> ConfiguredArtifact::ToArtifactName(const StringPiece& format,
                                                      const StringPiece& apk_name,
                                                      IDiagnostics* diag) const {
  Maybe<std::string> base = ToBaseName(format.to_string(), apk_name, diag);
  if (!base) {
    return {};
  }
  std::string result = std::move(base.value());

  if (!ReplacePlaceholder("${abi}", abi_group, &result, diag)) {
    return {};
  }

  if (!ReplacePlaceholder("${density}", screen_density_group, &result, diag)) {
    return {};
  }

  if (!ReplacePlaceholder("${locale}", locale_group, &result, diag)) {
    return {};
  }

  if (!ReplacePlaceholder("${sdk}", android_sdk_group, &result, diag)) {
    return {};
  }

  if (!ReplacePlaceholder("${feature}", device_feature_group, &result, diag)) {
    return {};
  }

  if (!ReplacePlaceholder("${gl}", gl_texture_group, &result, diag)) {
    return {};
  }

  return result;
}

Maybe<std::string> ConfiguredArtifact::Name(const StringPiece& apk_name, IDiagnostics* diag) const {
  if (!name) {
    return {};
  }

  return ToBaseName(name.value(), apk_name, diag);
}

}  // namespace configuration

/** Returns a ConfigurationParser for the file located at the provided path. */
Maybe<ConfigurationParser> ConfigurationParser::ForPath(const std::string& path) {
  std::string contents;
  if (!ReadFileToString(path, &contents, true)) {
    return {};
  }
  return ConfigurationParser(contents);
}

ConfigurationParser::ConfigurationParser(std::string contents)
    : contents_(std::move(contents)),
      diag_(&noop_) {
}

Maybe<std::vector<OutputArtifact>> ConfigurationParser::Parse(
    const android::StringPiece& apk_path) {
  Maybe<PostProcessingConfiguration> maybe_config = ExtractConfiguration(contents_, diag_);
  if (!maybe_config) {
    return {};
  }
  const PostProcessingConfiguration& config = maybe_config.value();

  // TODO: Automatically arrange artifacts so that they match Play Store multi-APK requirements.
  // see: https://developer.android.com/google/play/publishing/multiple-apks.html
  //
  // For now, make sure the version codes are unique.
  std::vector<ConfiguredArtifact> artifacts = config.artifacts;
  std::sort(artifacts.begin(), artifacts.end());
  if (std::adjacent_find(artifacts.begin(), artifacts.end()) != artifacts.end()) {
    diag_->Error(DiagMessage() << "Configuration has duplicate versions");
    return {};
  }

  const std::string& apk_name = file::GetFilename(apk_path).to_string();
  const StringPiece ext = file::GetExtension(apk_name);
  const std::string base_name = apk_name.substr(0, apk_name.size() - ext.size());

  // Convert from a parsed configuration to a list of artifacts for processing.
  std::vector<OutputArtifact> output_artifacts;
  bool has_errors = false;

  for (const ConfiguredArtifact& artifact : artifacts) {
    Maybe<OutputArtifact> output_artifact = ToOutputArtifact(artifact, apk_name, config, diag_);
    if (!output_artifact) {
      // Defer return an error condition so that all errors are reported.
      has_errors = true;
    } else {
      output_artifacts.push_back(std::move(output_artifact.value()));
    }
  }

  if (has_errors) {
    return {};
  }
  return {output_artifacts};
}

namespace configuration {
namespace handler {

bool ArtifactTagHandler(PostProcessingConfiguration* config, Element* root_element,
                        IDiagnostics* diag) {
  // This will be incremented later so the first version will always be different to the base APK.
  int current_version = (config->artifacts.empty()) ? 0 : config->artifacts.back().version;

  ConfiguredArtifact artifact{};
  Maybe<int> version;
  for (const auto& attr : root_element->attributes) {
    if (attr.name == "name") {
      artifact.name = attr.value;
    } else if (attr.name == "version") {
      version = std::stoi(attr.value);
    } else if (attr.name == "abi-group") {
      artifact.abi_group = {attr.value};
    } else if (attr.name == "screen-density-group") {
      artifact.screen_density_group = {attr.value};
    } else if (attr.name == "locale-group") {
      artifact.locale_group = {attr.value};
    } else if (attr.name == "android-sdk-group") {
      artifact.android_sdk_group = {attr.value};
    } else if (attr.name == "gl-texture-group") {
      artifact.gl_texture_group = {attr.value};
    } else if (attr.name == "device-feature-group") {
      artifact.device_feature_group = {attr.value};
    } else {
      diag->Note(DiagMessage() << "Unknown artifact attribute: " << attr.name << " = "
                               << attr.value);
    }
  }

  artifact.version = (version) ? version.value() : current_version + 1;

  config->artifacts.push_back(artifact);
  return true;
};

bool ArtifactFormatTagHandler(PostProcessingConfiguration* config, Element* root_element,
                              IDiagnostics* /* diag */) {
  for (auto& node : root_element->children) {
    xml::Text* t;
    if ((t = NodeCast<xml::Text>(node.get())) != nullptr) {
      config->artifact_format = TrimWhitespace(t->text).to_string();
      break;
    }
  }
  return true;
};

bool AbiGroupTagHandler(PostProcessingConfiguration* config, Element* root_element,
                        IDiagnostics* diag) {
  std::string label = GetLabel(root_element, diag);
  if (label.empty()) {
    return false;
  }

  auto& group = config->abi_groups[label];
  bool valid = true;

  for (auto* child : root_element->GetChildElements()) {
    if (child->name != "abi") {
      diag->Error(DiagMessage() << "Unexpected element in ABI group: " << child->name);
      valid = false;
    } else {
      for (auto& node : child->children) {
        xml::Text* t;
        if ((t = NodeCast<xml::Text>(node.get())) != nullptr) {
          group.push_back(kStringToAbiMap.at(TrimWhitespace(t->text).to_string()));
          break;
        }
      }
    }
  }

  return valid;
};

bool ScreenDensityGroupTagHandler(PostProcessingConfiguration* config, Element* root_element,
                                  IDiagnostics* diag) {
  std::string label = GetLabel(root_element, diag);
  if (label.empty()) {
    return false;
  }

  auto& group = config->screen_density_groups[label];
  bool valid = true;

  for (auto* child : root_element->GetChildElements()) {
    if (child->name != "screen-density") {
      diag->Error(DiagMessage() << "Unexpected root_element in screen density group: "
                                << child->name);
      valid = false;
    } else {
      for (auto& node : child->children) {
        xml::Text* t;
        if ((t = NodeCast<xml::Text>(node.get())) != nullptr) {
          ConfigDescription config_descriptor;
          const android::StringPiece& text = TrimWhitespace(t->text);
          bool parsed = ConfigDescription::Parse(text, &config_descriptor);
          if (parsed &&
              (config_descriptor.CopyWithoutSdkVersion().diff(ConfigDescription::DefaultConfig()) ==
               android::ResTable_config::CONFIG_DENSITY)) {
            // Copy the density with the minimum SDK version stripped out.
            group.push_back(config_descriptor.CopyWithoutSdkVersion());
          } else {
            diag->Error(DiagMessage()
                        << "Could not parse config descriptor for screen-density: " << text);
            valid = false;
          }
          break;
        }
      }
    }
  }

  return valid;
};

bool LocaleGroupTagHandler(PostProcessingConfiguration* config, Element* root_element,
                           IDiagnostics* diag) {
  std::string label = GetLabel(root_element, diag);
  if (label.empty()) {
    return false;
  }

  auto& group = config->locale_groups[label];
  bool valid = true;

  for (auto* child : root_element->GetChildElements()) {
    if (child->name != "locale") {
      diag->Error(DiagMessage() << "Unexpected root_element in screen density group: "
                                << child->name);
      valid = false;
    } else {
      for (auto& node : child->children) {
        xml::Text* t;
        if ((t = NodeCast<xml::Text>(node.get())) != nullptr) {
          ConfigDescription config_descriptor;
          const android::StringPiece& text = TrimWhitespace(t->text);
          bool parsed = ConfigDescription::Parse(text, &config_descriptor);
          if (parsed &&
              (config_descriptor.CopyWithoutSdkVersion().diff(ConfigDescription::DefaultConfig()) ==
               android::ResTable_config::CONFIG_LOCALE)) {
            // Copy the locale with the minimum SDK version stripped out.
            group.push_back(config_descriptor.CopyWithoutSdkVersion());
          } else {
            diag->Error(DiagMessage()
                        << "Could not parse config descriptor for screen-density: " << text);
            valid = false;
          }
          break;
        }
      }
    }
  }

  return valid;
};

bool AndroidSdkGroupTagHandler(PostProcessingConfiguration* config, Element* root_element,
                               IDiagnostics* diag) {
  std::string label = GetLabel(root_element, diag);
  if (label.empty()) {
    return false;
  }

  bool valid = true;
  bool found = false;

  for (auto* child : root_element->GetChildElements()) {
    if (child->name != "android-sdk") {
      diag->Error(DiagMessage() << "Unexpected root_element in ABI group: " << child->name);
      valid = false;
    } else {
      AndroidSdk entry;
      for (const auto& attr : child->attributes) {
        Maybe<int>* target = nullptr;
        if (attr.name == "minSdkVersion") {
          target = &entry.min_sdk_version;
        } else if (attr.name == "targetSdkVersion") {
          target = &entry.target_sdk_version;
        } else if (attr.name == "maxSdkVersion") {
          target = &entry.max_sdk_version;
        } else {
          diag->Warn(DiagMessage() << "Unknown attribute: " << attr.name << " = " << attr.value);
          continue;
        }

        *target = ResourceUtils::ParseSdkVersion(attr.value);
        if (!*target) {
          diag->Error(DiagMessage() << "Invalid attribute: " << attr.name << " = " << attr.value);
          valid = false;
        }
      }

      // TODO: Fill in the manifest details when they are finalised.
      for (auto node : child->GetChildElements()) {
        if (node->name == "manifest") {
          if (entry.manifest) {
            diag->Warn(DiagMessage() << "Found multiple manifest tags. Ignoring duplicates.");
            continue;
          }
          entry.manifest = {AndroidManifest()};
        }
      }

      config->android_sdk_groups[label] = entry;
      if (found) {
        valid = false;
      }
      found = true;
    }
  }

  return valid;
};

bool GlTextureGroupTagHandler(PostProcessingConfiguration* config, Element* root_element,
                              IDiagnostics* diag) {
  std::string label = GetLabel(root_element, diag);
  if (label.empty()) {
    return false;
  }

  auto& group = config->gl_texture_groups[label];
  bool valid = true;

  GlTexture result;
  for (auto* child : root_element->GetChildElements()) {
    if (child->name != "gl-texture") {
      diag->Error(DiagMessage() << "Unexpected element in GL texture group: " << child->name);
      valid = false;
    } else {
      for (const auto& attr : child->attributes) {
        if (attr.name == "name") {
          result.name = attr.value;
          break;
        }
      }

      for (auto* element : child->GetChildElements()) {
        if (element->name != "texture-path") {
          diag->Error(DiagMessage() << "Unexpected element in gl-texture element: " << child->name);
          valid = false;
          continue;
        }
        for (auto& node : element->children) {
          xml::Text* t;
          if ((t = NodeCast<xml::Text>(node.get())) != nullptr) {
            result.texture_paths.push_back(TrimWhitespace(t->text).to_string());
          }
        }
      }
    }
    group.push_back(result);
  }

  return valid;
};

bool DeviceFeatureGroupTagHandler(PostProcessingConfiguration* config, Element* root_element,
                                  IDiagnostics* diag) {
  std::string label = GetLabel(root_element, diag);
  if (label.empty()) {
    return false;
  }

  auto& group = config->device_feature_groups[label];
  bool valid = true;

  for (auto* child : root_element->GetChildElements()) {
    if (child->name != "supports-feature") {
      diag->Error(DiagMessage() << "Unexpected root_element in device feature group: "
                                << child->name);
      valid = false;
    } else {
      for (auto& node : child->children) {
        xml::Text* t;
        if ((t = NodeCast<xml::Text>(node.get())) != nullptr) {
          group.push_back(TrimWhitespace(t->text).to_string());
          break;
        }
      }
    }
  }

  return valid;
};

}  // namespace handler
}  // namespace configuration

}  // namespace aapt
