// Host tests for the native egg.json parser.
//
// AntEggLoader had no host test at all, which is how the two parsers - the Kotlin one in the
// launcher and this one in the preloader - could drift without anything failing. The struct's own
// doc-comment says both "must apply the same rules or a package could be accepted in one place and
// rejected in the other"; these tests are that claim, checked.
//
// ParseManifest is a pure static that takes JSON text, so it needs no device and no archive.

#include "pl/runtime/AntEggLoader.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
  }
}

std::string NativeManifest(const std::string& extra_fields = "") {
  std::string json =
      "{"
      "\"name\":\"Test Mod\","
      "\"version\":\"1.2.3\","
      "\"author\":\"Someone\","
      "\"type\":\"native\","
      "\"entry_point\":\"lib/libtest.so\"";
  if (!extra_fields.empty()) json += "," + extra_fields;
  json += "}";
  return json;
}

std::string ScriptManifest(const std::string& extra_fields = "") {
  std::string json =
      "{"
      "\"name\":\"Script Mod\","
      "\"version\":\"0.1.0\","
      "\"author\":\"Someone\","
      "\"type\":\"script\","
      "\"entry_point\":\"main.lua\"";
  if (!extra_fields.empty()) json += "," + extra_fields;
  json += "}";
  return json;
}

void ParsesAValidNativeManifest() {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(NativeManifest(), manifest, error),
        "a well-formed native manifest parses: " + error);
  Check(manifest.name == "Test Mod", "name is read");
  Check(manifest.version == "1.2.3", "version is read");
  Check(manifest.author == "Someone", "author is read");
  Check(manifest.IsNative(), "type native is recognised");
  Check(!manifest.IsScript(), "a native manifest is not a script");
  Check(manifest.entry_point == "lib/libtest.so", "entry_point is read");
  Check(manifest.Id() == "test-mod", "id is the slug of the name");
}

void ParsesAValidScriptManifest() {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(ScriptManifest(), manifest, error),
        "a well-formed script manifest parses: " + error);
  Check(manifest.IsScript(), "type script is recognised");
  Check(manifest.entry_point == "main.lua", "the lua entry point is read");
}

void ReadsThePermissionsSlot() {
  // Item C3. The Java writer always emits this key, so a parser that ignored it would be reading a
  // manifest the other side describes differently.
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(
            NativeManifest("\"permissions\":[\"filesystem.read\",\"network\"]"), manifest, error),
        "a manifest with permissions parses: " + error);
  Check(manifest.permissions.size() == 2, "both permissions are read");
  Check(!manifest.permissions.empty() && manifest.permissions[0] == "filesystem.read",
        "the first permission is read in order");
  Check(manifest.permissions.size() > 1 && manifest.permissions[1] == "network",
        "the second permission is read in order");
}

void AnEmptyPermissionsArrayIsValidAndReadsAsEmpty() {
  // This is exactly what the Java writer emits for a mod that declares nothing, so it must not be
  // an error.
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(
            NativeManifest("\"permissions\":[]"), manifest, error),
        "an empty permissions array is accepted: " + error);
  Check(manifest.permissions.empty(), "an empty array reads as no permissions");
}

void AnOmittedPermissionsSlotIsAccepted() {
  // A hand-written package predating the slot must still load.
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(NativeManifest(), manifest, error),
        "a manifest without permissions is accepted: " + error);
  Check(manifest.permissions.empty(), "an omitted slot reads as no permissions");
}

void AMalformedPermissionsSlotIsRefused() {
  // Mirrors the Kotlin rule: an array of capability names, so a scalar is a manifest error rather
  // than something silently ignored.
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(!pl::runtime::AntEggLoader::ParseManifest(
            NativeManifest("\"permissions\":\"network\""), manifest, error),
        "a scalar permissions value is refused");
  Check(error.find("permissions") != std::string::npos,
        "the error names the permissions field");

  pl::runtime::AntEggManifest manifest2;
  std::string error2;
  Check(!pl::runtime::AntEggLoader::ParseManifest(
            NativeManifest("\"permissions\":[123]"), manifest2, error2),
        "a non-string permission entry is refused");
}

void BlankPermissionsAreDropped() {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(
            NativeManifest("\"permissions\":[\"\",\"  \",\"network\"]"), manifest, error),
        "blank entries do not fail the parse: " + error);
  Check(manifest.permissions.size() == 1, "only the non-blank permission is kept");
  Check(!manifest.permissions.empty() && manifest.permissions[0] == "network",
        "the kept permission is the non-blank one");
}

void ReadsDependencies() {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(
            NativeManifest("\"dependencies\":[\"core\",\"lib\"]"), manifest, error),
        "a manifest with dependencies parses: " + error);
  Check(manifest.dependencies.size() == 2, "both dependencies are read");
  Check(manifest.dependencies.size() > 1 && manifest.dependencies[1] == "lib",
        "dependencies keep their order");
}

void AnUnknownKeyIsTolerated() {
  // Forward compatibility: the Java writer emits keys this parser may not know about yet
  // (ai_ported, origin_mod, sha256). Ignoring an unknown key is what stops a newer package from
  // failing to load on an older build.
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(pl::runtime::AntEggLoader::ParseManifest(
            NativeManifest("\"sha256\":\"abc\",\"ai_ported\":true,\"origin_mod\":\"x.jar\""),
            manifest, error),
        "unknown keys do not fail the parse: " + error);
}

void RequiredFieldsAreEnforced() {
  struct Case {
    const char* json;
    const char* what;
  };
  const Case cases[] = {
      {"{\"version\":\"1.0.0\",\"author\":\"a\",\"type\":\"native\",\"entry_point\":\"x.so\"}",
       "a missing name is refused"},
      {"{\"name\":\"n\",\"author\":\"a\",\"type\":\"native\",\"entry_point\":\"x.so\"}",
       "a missing version is refused"},
      {"{\"name\":\"n\",\"version\":\"1.0.0\",\"type\":\"native\",\"entry_point\":\"x.so\"}",
       "a missing author is refused"},
      {"{\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"a\",\"entry_point\":\"x.so\"}",
       "a missing type is refused"},
      {"{\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"a\",\"type\":\"native\"}",
       "a missing entry_point is refused"},
  };
  for (const Case& c : cases) {
    pl::runtime::AntEggManifest manifest;
    std::string error;
    Check(!pl::runtime::AntEggLoader::ParseManifest(c.json, manifest, error), c.what);
    Check(!error.empty(), std::string(c.what) + " and reports why");
  }
}

void TypeAndEntryPointMustAgree() {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(!pl::runtime::AntEggLoader::ParseManifest(
            "{\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"a\",\"type\":\"native\","
            "\"entry_point\":\"main.lua\"}",
            manifest, error),
        "a native mod with a .lua entry point is refused");

  pl::runtime::AntEggManifest manifest2;
  std::string error2;
  Check(!pl::runtime::AntEggLoader::ParseManifest(
            "{\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"a\",\"type\":\"script\","
            "\"entry_point\":\"lib.so\"}",
            manifest2, error2),
        "a script mod with a .so entry point is refused");
}

void AnEntryPointCannotEscapeThePackage() {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(!pl::runtime::AntEggLoader::ParseManifest(
            "{\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"a\",\"type\":\"native\","
            "\"entry_point\":\"../../evil.so\"}",
            manifest, error),
        "an entry point escaping the package is refused");

  pl::runtime::AntEggManifest manifest2;
  std::string error2;
  Check(!pl::runtime::AntEggLoader::ParseManifest(
            "{\"name\":\"n\",\"version\":\"1.0.0\",\"author\":\"a\",\"type\":\"native\","
            "\"entry_point\":\"/abs/evil.so\"}",
            manifest2, error2),
        "an absolute entry point is refused");
}

void AMalformedDocumentIsRefused() {
  pl::runtime::AntEggManifest manifest;
  std::string error;
  Check(!pl::runtime::AntEggLoader::ParseManifest("not json at all", manifest, error),
        "non-JSON is refused");
  Check(!error.empty(), "a malformed document reports why");

  pl::runtime::AntEggManifest manifest2;
  std::string error2;
  Check(!pl::runtime::AntEggLoader::ParseManifest("[1,2,3]", manifest2, error2),
        "a JSON array is not a manifest");

  pl::runtime::AntEggManifest manifest3;
  std::string error3;
  Check(!pl::runtime::AntEggLoader::ParseManifest(
            "{\"name\":\"n\",\"version\":\"not-a-version\",\"author\":\"a\",\"type\":\"native\","
            "\"entry_point\":\"x.so\"}",
            manifest3, error3),
        "a non-semver version is refused");
}

void LooksLikeAntEggIsCaseInsensitive() {
  Check(pl::runtime::AntEggLoader::LooksLikeAntEgg("mod.antegg"), ".antegg is recognised");
  Check(pl::runtime::AntEggLoader::LooksLikeAntEgg("MOD.ANTEGG"), "the extension is case-insensitive");
  Check(!pl::runtime::AntEggLoader::LooksLikeAntEgg("mod.zip"), ".zip is not an .antegg");
  Check(!pl::runtime::AntEggLoader::LooksLikeAntEgg("antegg"), "a bare name is not an .antegg");
}

}  // namespace

int main() {
  ParsesAValidNativeManifest();
  ParsesAValidScriptManifest();
  ReadsThePermissionsSlot();
  AnEmptyPermissionsArrayIsValidAndReadsAsEmpty();
  AnOmittedPermissionsSlotIsAccepted();
  AMalformedPermissionsSlotIsRefused();
  BlankPermissionsAreDropped();
  ReadsDependencies();
  AnUnknownKeyIsTolerated();
  RequiredFieldsAreEnforced();
  TypeAndEntryPointMustAgree();
  AnEntryPointCannotEscapeThePackage();
  AMalformedDocumentIsRefused();
  LooksLikeAntEggIsCaseInsensitive();

  if (g_failures == 0) {
    std::printf("AntEggManifestTest: all %d checks passed\n", g_checks);
    return 0;
  }
  std::fprintf(stderr, "AntEggManifestTest: %d of %d checks FAILED\n", g_failures, g_checks);
  return 1;
}
