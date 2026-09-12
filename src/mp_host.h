// Binds the shared LAN multiplayer code (src/mp) into the loader.
#pragma once

namespace mp {
// Applies the game patches and registers the HLE stubs. Call after s3e_load()
// and before the entry point, while the JIT code cache is still empty.
void init();
void shutdown();
}  // namespace mp
