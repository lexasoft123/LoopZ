#include "../../native/platform/macos/desktop/desktop.hpp"
#include <cassert>

int main() {
    @autoreleasepool {
        for (NSString* url in
             @[ @"loopz://app/index.html", @"loopz://app/loopz.js", @"loopz://app/loopz.css" ])
            assert(loopzLocalUIURL([NSURL URLWithString:url]));
        for (NSString* url in @[
                 @"https://app/index.html", @"file:///index.html", @"loopz://other/index.html",
                 @"loopz://app:12/index.html", @"loopz://user@app/index.html",
                 @"loopz://app/index.html?x=1", @"loopz://app/index.html#anchor",
                 @"loopz://app/%69ndex.html", @"loopz://app/../index.html",
                 @"loopz://app/manifest.json", @"loopz://app/etc/passwd"
             ])
            assert(!loopzLocalUIURL([NSURL URLWithString:url]));
        assert(!loopzLocalUIURL(nil));
    }
}
