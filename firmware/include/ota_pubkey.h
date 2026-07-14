// OTA image-signing public key (RSA-2048). SAFE TO COMMIT — public half only.
// The matching private key lives in infra/keys/ (gitignored) on the build host.
// The device verifies firmware.bin.sig against this key before installing an OTA.
#pragma once
static const char OTA_PUBKEY_PEM[] =
  "-----BEGIN PUBLIC KEY-----\n"
  "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0pVTYjw8qKbrAVEbcRRq\n"
  "nvSEit9wpftkR8rpyvl4s2qSx3rhfu0zItlSAc/Jweo/niVoT8DH2BWbXHGLZ/6u\n"
  "/s367j1zM3b1NjVcQ3dGGLSYQGP+pc56esj3pKRfsB1C9FWMYe+K9vo1cPgbaHw9\n"
  "8tNdkQWBIY8QSfaJ30aH5xzijemouJJZ4zrYcIhCjNoNnAOSE6QNH4CwXIKx08CP\n"
  "xXcVS9saDwDlET9GbKCYoH5/7LlOsem5eFSQLm3PRpdhWrc7Umjhe8/zQIjKlgF8\n"
  "TUWdxbTaJrbal8J//f8yV09NQ49SS8o8QwwFY86jf9Uktz0S8iWXQk67y5pF9SLr\n"
  "cwIDAQAB\n"
  "-----END PUBLIC KEY-----\n"
  ;
