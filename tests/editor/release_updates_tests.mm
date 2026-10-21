#include "../../native/platform/macos/desktop/release_updates.hpp"
#include <cassert>
int main() {
    @autoreleasepool {
        assert(loopzNewerStableRelease(@"v0.1.1", @"0.1.0"));
        assert(loopzNewerStableRelease(@"v0.10.0", @"0.9.0"));
        assert(loopzNewerStableRelease(@"1.0.0", @"0.99.99"));
        assert(!loopzNewerStableRelease(@"v0.1.0", @"0.1.0"));
        assert(!loopzNewerStableRelease(@"0.0.9", @"0.1.0"));
        for (NSString* invalid in @[
                 @"", @"v1.2", @"1.2.3.4", @"1.2.3-beta", @"01.2.3", @"1.2.foo",
                 @"1.9999999999999999999.0"
             ])
            assert(!loopzNewerStableRelease(invalid, @"0.1.0"));
        assert(!loopzNewerStableRelease((id) @{}, @"0.1.0"));
        assert(!loopzNewerStableRelease(@"1.0.0", nil));
    }
}
