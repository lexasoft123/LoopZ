#include <loopz/document_json.hpp>
#include <cassert>
#include <iostream>

using namespace loopz;
template <class F> void rejects(F f) {
    bool caught = false;
    try {
        f();
    } catch (const std::exception&) {
        caught = true;
    }
    assert(caught);
}
int main() {
    const auto commands = parseControlJSON(R"([
      {"type":"createDevice","id":"mix","name":"Русский 混音","passThrough":false},
      {"type":"addSource","device":"mix","id":"mic","kind":"input-device","selector":"default","channels":2},
      {"type":"setSourceControls","device":"mix","source":"mic","controls":{"gain":0.25}},
      {"type":"setSourceWiring","device":"mix","source":"mic","connections":[{"sourceChannel":1,"outputChannel":0,"gain":0.5}]},
      {"type":"addMonitor","device":"mix","id":"phones","selector":"endpoint","channels":2},
      {"type":"setMonitorControls","device":"mix","monitor":"phones","controls":{"muted":true}}
    ])");
    auto document = edit::applyCommands({}, decodeCommands(commands));
    const auto encoded = encodeDocument(document);
    assert(encodeDocument(decodeDocument(parseControlJSON(encoded.dump()))) == encoded);
    assert(document.devices[0].routing.sources[0].gain == 0.25f);
    assert(document.devices[0].routing.sources[0].connections[0].sourceChannel == 1);
    auto bad = encoded;
    bad["devices"][0]["routing"]["gain"] = true;
    rejects([&] {
        (void)decodeDocument(bad);
    });
    bad = encoded;
    bad["devices"][0]["extra"] = 1;
    rejects([&] {
        (void)decodeDocument(bad);
    });
    rejects([&] {
        (void)decodeCommands(
            JSON::parse(R"([{"type":"createDevice","id":"x","name":"x","channels":2.5}])"));
    });
    rejects([&] {
        (void)decodeCommands(
            JSON::parse(R"([{"type":"createDevice","id":"x","name":"x","passThrough":1}])"));
    });
    rejects([&] {
        (void)decodeCommands(
            JSON::parse(R"([{"type":"deleteDevice","device":"mix","typo":true}])"));
    });
    rejects([&] {
        (void)parseControlJSON(std::string(1024 * 1024 + 1, ' '));
    });
    rejects([&] {
        (void)parseControlJSON(std::string(40, '[') + std::string(40, ']'));
    });
    // Decoding must enforce graph invariants, not merely JSON field types.
    bad = encoded;
    bad["version"] = kRouteDocumentVersion + 1;
    rejects([&] {
        (void)decodeDocument(bad);
    });
    bad = encoded;
    bad["devices"][0]["routing"]["sources"][0]["connections"][0]["sourceChannel"] = 2;
    rejects([&] {
        (void)decodeDocument(bad);
    });
    bad = encoded;
    bad["devices"][0]["bindings"][0]["kind"] = "unknown-source";
    rejects([&] {
        (void)decodeDocument(bad);
    });
    auto oversizedCommands = JSON::array();
    for (size_t i = 0; i <= edit::kMaximumCommands; ++i)
        oversizedCommands.push_back({{"type", "deleteDevice"}, {"device", "mix"}});
    rejects([&] {
        (void)decodeCommands(oversizedCommands);
    });
    // Invalid edits leave the original document unchanged.
    const auto original = encodeDocument(document);
    rejects([&] {
        (void)edit::applyCommands(document,
                                  decodeCommands(JSON::parse(
                                      R"([{"type":"renameDevice","device":"mix","name":"Changed"},
                {"type":"setSourceWiring","device":"mix","source":"mic","connections":[{"sourceChannel":9,"outputChannel":0,"gain":1}]}])")));
    });
    assert(encodeDocument(document) == original);
    std::cout << "PASS: portable document JSON, Unicode, commands and malformed input\n";
}
