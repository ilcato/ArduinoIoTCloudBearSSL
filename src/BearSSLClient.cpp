/*
 * This file is part of ArduinoIoTBearSSL.
 *
 * Copyright 2019 ARDUINO SA (http://www.arduino.cc/)
 *
 * This software is released under the GNU General Public License version 3,
 * which covers the main part of ArduinoIoTBearSSL.
 * The terms of this license can be found at:
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * You can be released from the requirements of the above licenses by purchasing
 * a commercial license. Buying such a license is mandatory if you want to modify or
 * otherwise use the software for commercial activities involving the Arduino
 * software without disclosing the source code of your own applications. To purchase
 * a commercial license, send an email to license@arduino.cc.
 *
 */

#include <ArduinoECCX08.h>

#include "ArduinoIoTCloudBearSSL.h"
#include "BearSSLServerCertificate.h"
#include "utility/eccX08_asn1.h"

#include "BearSSLClient.h"

BearSSLClient::BearSSLClient(Client& client) :
  _client(&client)
{
  _ecKey.curve = 0;
  _ecKey.x = NULL;
  _ecKey.xlen = 0;

  _ecCert.data = NULL;
  _ecCert.data_len = 0;
}

BearSSLClient::~BearSSLClient()
{
}

int BearSSLClient::connect(IPAddress ip, uint16_t port)
{
  if (!_client->connect(ip, port)) {
    return 0;
  }

  return connectSSL(NULL);
}

int BearSSLClient::connect(const char* host, uint16_t port)
{
  if (!_client->connect(host, port)) {
    return 0;
  }

  return connectSSL(host);
}

size_t BearSSLClient::write(uint8_t b)
{
  return write(&b, sizeof(b));
}

size_t BearSSLClient::write(const uint8_t *buf, size_t size)
{
  size_t written = 0;

  while (written < size) {
    int result = br_sslio_write(&_ioc, buf, size);

    if (result < 0) {
      break;
    }

    buf += result;
    written += result;
  }

  if (written == size && br_sslio_flush(&_ioc) < 0) {
    return 0;
  }

  return written;
}

int BearSSLClient::available()
{
  int available = br_sslio_read_available(&_ioc);

  if (available < 0) {
    available = 0;
  }

  return available;
}

int BearSSLClient::read()
{
  byte b;

  if (read(&b, sizeof(b)) == sizeof(b)) {
    return b;
  }

  return -1;
}

int BearSSLClient::read(uint8_t *buf, size_t size)
{
  return br_sslio_read(&_ioc, buf, size);
}

int BearSSLClient::peek()
{
  byte b;

  if (br_sslio_peek(&_ioc, &b, sizeof(b)) == sizeof(b)) {
    return b;
  }

  return -1;
}

void BearSSLClient::flush()
{
  br_sslio_flush(&_ioc);

  _client->flush();
}

void BearSSLClient::stop()
{
  if (_client->connected()) {
    if ((br_ssl_engine_current_state(&_sc.eng) & BR_SSL_CLOSED) == 0) {
      br_sslio_close(&_ioc);
    }

    _client->stop();
  }
}

uint8_t BearSSLClient::connected()
{
  if (!_client->connected()) {
    return 0;
  }

  unsigned state = br_ssl_engine_current_state(&_sc.eng);

  if (state == BR_SSL_CLOSED) {
    return 0;
  }

  return 1;
}

BearSSLClient::operator bool()
{
  return (*_client);  
}

void BearSSLClient::setEccSlot(int ecc508KeySlot, const byte cert[], int certLength)
{
  // HACK: put the key slot info. in the br_ec_private_key structure
  _ecKey.curve = 23;
  _ecKey.x = (unsigned char*)ecc508KeySlot;
  _ecKey.xlen = 32;

  _ecCert.data = (unsigned char*)cert;
  _ecCert.data_len = certLength;
}

int BearSSLClient::connectSSL(const char* host)
{
  // initialize client context with all algorithms and known server publick key
  br_ec_public_key public_key;
  public_key.curve = BR_EC_secp256r1;
  public_key.q = (unsigned char *)TA0_EC_Q;
  public_key.qlen = sizeof TA0_EC_Q;
  arduino_client_profile(&_sc, &_xc, &public_key);

  // set the buffer in split mode
  br_ssl_engine_set_buffer(&_sc.eng, _iobuf, sizeof(_iobuf), 1);

  // inject entropy in engine
  unsigned char entropy[32];

  if (ECCX08.begin() && ECCX08.locked() && ECCX08.random(entropy, sizeof(entropy))) {
    // ECC508 random success, add custom ECDSA vfry and EC sign
    br_ssl_engine_set_ecdsa(&_sc.eng, eccX08_vrfy_asn1);
    // enable client auth using the ECCX08
    if (_ecCert.data_len && _ecKey.xlen) {
      br_ssl_client_set_single_ec(&_sc, &_ecCert, 1, &_ecKey, BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN, BR_KEYTYPE_EC, br_ec_get_default(), eccX08_sign_asn1);
    }
  } else {
    // no ECCX08 or random failed, fallback to pseudo random
    for (size_t i = 0; i < sizeof(entropy); i++) {
      entropy[i] = random(0, 255);
    }
  }
  br_ssl_engine_inject_entropy(&_sc.eng, entropy, sizeof(entropy));

  // set the hostname used for SNI
  br_ssl_client_reset(&_sc, host, 0);

  // use our own socket I/O operations
  br_sslio_init(&_ioc, &_sc.eng, BearSSLClient::clientRead, _client, BearSSLClient::clientWrite, _client);

  br_sslio_flush(&_ioc);

  while (1) {
    unsigned state = br_ssl_engine_current_state(&_sc.eng);

    if (state & BR_SSL_SENDAPP) {
      break;
    } else if (state & BR_SSL_CLOSED) {
      return 0;
    }
  }

  return 1;
}

// #define DEBUGSERIAL Serial

int BearSSLClient::clientRead(void *ctx, unsigned char *buf, size_t len)
{
  Client* c = (Client*)ctx;

  if (!c->connected()) {
    return -1;
  }

  int result = c->read(buf, len);
  if (result == -1) {
    return 0;
  }

#ifdef DEBUGSERIAL
  DEBUGSERIAL.print("BearSSLClient::clientRead - ");
  DEBUGSERIAL.print(result);
  DEBUGSERIAL.print(" - ");  
  for (size_t i = 0; i < result; i++) {
    byte b = buf[i];

    if (b < 16) {
      DEBUGSERIAL.print("0");
    }
    DEBUGSERIAL.print(b, HEX);
  }
  DEBUGSERIAL.println();
#endif

  return result;
}

int BearSSLClient::clientWrite(void *ctx, const unsigned char *buf, size_t len)
{
  Client* c = (Client*)ctx;

#ifdef DEBUGSERIAL
  DEBUGSERIAL.print("BearSSLClient::clientWrite - ");
  DEBUGSERIAL.print(len);
  DEBUGSERIAL.print(" - ");
  for (size_t i = 0; i < len; i++) {
    byte b = buf[i];

    if (b < 16) {
      DEBUGSERIAL.print("0");
    }
    DEBUGSERIAL.print(b, HEX);
  }
  DEBUGSERIAL.println();
#endif

  if (!c->connected()) {
    return -1;
  }

  int result = c->write(buf, len);
  if (result == 0) {
    return -1;
  }

  return result;
}
