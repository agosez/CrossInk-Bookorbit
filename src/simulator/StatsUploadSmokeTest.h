#pragma once
#ifdef SIMULATOR
// Runs only under the explicit simulator smoke-test flag, with isolated storage.
void verifyStatsUploadContract();
#endif
