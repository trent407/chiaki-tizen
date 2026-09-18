// chiaki-ng mainline initializes AIA certificate-chain recovery from
// chiaki_lib_init(). The Tizen WASM port does not compile upstream's
// curl/OpenSSL holepunch stack, so keep the global init contract satisfied
// without linking libcurl into the TV module.

#include <chiaki/common.h>

ChiakiErrorCode chiaki_aia_init(void)
{
	return CHIAKI_ERR_SUCCESS;
}

void chiaki_aia_fini(void)
{
}
