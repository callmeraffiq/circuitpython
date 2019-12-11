/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
 * Copyright (c) 2019 Jeff Epler for Adafruit Industries
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "shared-bindings/audiomp3/MP3File.h"

#include <stdint.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#include "shared-module/audiomp3/MP3File.h"
#include "supervisor/shared/translate.h"
#include "lib/mp3/src/mp3common.h"

/** Fill the input buffer if it is less than half full.
 *
 * Returns true if the input buffer contains any useful data,
 * false otherwise.  (The input buffer will be padded to the end with
 * 0 bytes, which do not interfere with MP3 decoding)
 *
 * Raises OSError if f_read fails.
 *
 * Sets self->eof if any read of the file returns 0 bytes
 */
STATIC bool mp3file_update_inbuf(audiomp3_mp3file_obj_t* self) {
    // If buffer is over half full, do nothing
    if (self->inbuf_offset < self->inbuf_length/2) return true;

    // If we didn't previously reach the end of file, we can try reading now
    if (!self->eof) {

        // Move the unconsumed portion of the buffer to the start
        uint8_t *end_of_buffer = self->inbuf + self->inbuf_length;
        uint8_t *new_end_of_data = self->inbuf + self->inbuf_length - self->inbuf_offset;
        memmove(self->inbuf, self->inbuf + self->inbuf_offset,
            self->inbuf_length - self->inbuf_offset);
        self->inbuf_offset = 0;

        UINT to_read = end_of_buffer - new_end_of_data;
        UINT bytes_read = 0;
        memset(new_end_of_data, 0, to_read);
        if (f_read(&self->file->fp, new_end_of_data, to_read, &bytes_read) != FR_OK) {
            self->eof = true;
            mp_raise_OSError(MP_EIO);
        }

        if (bytes_read == 0) {
            self->eof = true;
        }

        if (to_read != bytes_read) {
            new_end_of_data += bytes_read;
            memset(new_end_of_data, 0, end_of_buffer - new_end_of_data);
        }

    }

    // Return true iff there are at least some useful bytes in the buffer
    return self->inbuf_offset < self->inbuf_length;
}

#define READ_PTR(self) (self->inbuf + self->inbuf_offset)
#define BYTES_LEFT(self) (self->inbuf_length - self->inbuf_offset)
#define CONSUME(self, n) (self->inbuf_offset += n)

/* If a sync word can be found, advance to it and return true.  Otherwise,
 * return false.
 */
STATIC bool mp3file_find_sync_word(audiomp3_mp3file_obj_t* self) {
    do {
        mp3file_update_inbuf(self);
        int offset = MP3FindSyncWord(READ_PTR(self), BYTES_LEFT(self));
        if (offset >= 0) {
            CONSUME(self, offset);
            mp3file_update_inbuf(self);
            return true;
        }
        CONSUME(self, MAX(0, BYTES_LEFT(self) - 16));
    } while (!self->eof);
    return false;
}

STATIC bool mp3file_get_next_frame_info(audiomp3_mp3file_obj_t* self, MP3FrameInfo* fi) {
    int err = MP3GetNextFrameInfo(self->decoder, fi, READ_PTR(self));
    return err == ERR_MP3_NONE;
}

void common_hal_audiomp3_mp3file_construct(audiomp3_mp3file_obj_t* self,
                                           pyb_file_obj_t* file,
                                           uint8_t *buffer,
                                           size_t buffer_size) {
    // XXX Adafruit_MP3 uses a 2kB input buffer and two 4kB output buffers.
    // for a whopping total of 10kB buffers (+mp3 decoder state and frame buffer)
    // At 44kHz, that's 23ms of output audio data.
    //
    // We will choose a slightly different allocation strategy for the output:
    // Make sure the buffers are sized exactly to match (a multiple of) the
    // frame size; this is typically 2304 * 2 bytes, so a little bit bigger
    // than the two 4kB output buffers, except that the alignment allows to
    // never allocate that extra frame buffer.

    self->file = file;
    self->inbuf_length = 2048;
    self->inbuf_offset = self->inbuf_length;
    self->inbuf = m_malloc(self->inbuf_length, false);
    if (self->inbuf == NULL) {
        common_hal_audiomp3_mp3file_deinit(self);
        mp_raise_msg(&mp_type_MemoryError,
                     translate("Couldn't allocate input buffer"));
    }
    self->decoder = MP3InitDecoder();
    if (self->decoder == NULL) {
        common_hal_audiomp3_mp3file_deinit(self);
        mp_raise_msg(&mp_type_MemoryError,
                     translate("Couldn't allocate decoder"));
    }

    mp3file_find_sync_word(self);
    MP3FrameInfo fi;
    if(!mp3file_get_next_frame_info(self, &fi)) {
        mp_raise_msg(&mp_type_RuntimeError,
                     translate("Failed to parse MP3 file"));
    }

    self->sample_rate = fi.samprate;
    self->channel_count = fi.nChans;
    self->frame_buffer_size = fi.outputSamps*sizeof(int16_t);

    if (buffer_size >= 2 * self->frame_buffer_size) {
        self->len = buffer_size / 2 / self->frame_buffer_size * self->frame_buffer_size;
        self->buffers[0] = buffer;
        self->buffers[1] = buffer + self->len;
    } else {
        self->len = 2 * self->frame_buffer_size;
        self->buffers[0] = m_malloc(self->len, false);
        if (self->buffers[0] == NULL) {
            common_hal_audiomp3_mp3file_deinit(self);
            mp_raise_msg(&mp_type_MemoryError,
                         translate("Couldn't allocate first buffer"));
        }

        self->buffers[1] = m_malloc(self->len, false);
        if (self->buffers[1] == NULL) {
            common_hal_audiomp3_mp3file_deinit(self);
            mp_raise_msg(&mp_type_MemoryError,
                         translate("Couldn't allocate second buffer"));
        }
    }
}

void common_hal_audiomp3_mp3file_deinit(audiomp3_mp3file_obj_t* self) {
    MP3FreeDecoder(self->decoder);
    self->decoder = NULL;
    self->inbuf = NULL;
    self->buffers[0] = NULL;
    self->buffers[1] = NULL;
    self->file = NULL;
}

bool common_hal_audiomp3_mp3file_deinited(audiomp3_mp3file_obj_t* self) {
    return self->buffers[0] == NULL;
}

uint32_t common_hal_audiomp3_mp3file_get_sample_rate(audiomp3_mp3file_obj_t* self) {
    return self->sample_rate;
}

void common_hal_audiomp3_mp3file_set_sample_rate(audiomp3_mp3file_obj_t* self,
                                                 uint32_t sample_rate) {
    self->sample_rate = sample_rate;
}

uint8_t common_hal_audiomp3_mp3file_get_bits_per_sample(audiomp3_mp3file_obj_t* self) {
    return 16;
}

uint8_t common_hal_audiomp3_mp3file_get_channel_count(audiomp3_mp3file_obj_t* self) {
    return self->channel_count;
}

bool audiomp3_mp3file_samples_signed(audiomp3_mp3file_obj_t* self) {
    return true;
}

void audiomp3_mp3file_reset_buffer(audiomp3_mp3file_obj_t* self,
                                   bool single_channel,
                                   uint8_t channel) {
    if (single_channel && channel == 1) {
        return;
    }
    // We don't reset the buffer index in case we're looping and we have an odd number of buffer
    // loads
    f_lseek(&self->file->fp, 0);
    self->inbuf_offset = self->inbuf_length;
    self->eof = 0;
    mp3file_update_inbuf(self);
    mp3file_find_sync_word(self);
}

audioio_get_buffer_result_t audiomp3_mp3file_get_buffer(audiomp3_mp3file_obj_t* self,
                                                        bool single_channel,
                                                        uint8_t channel,
                                                        uint8_t** bufptr,
                                                        uint32_t* buffer_length) {
    if (!single_channel) {
        channel = 0;
    }

    uint16_t channel_read_count = self->channel_read_count[channel]++;
    bool need_more_data = self->read_count++ == channel_read_count;

    *bufptr = self->buffers[self->buffer_index] + channel;
    *buffer_length = self->frame_buffer_size;

    if (need_more_data) {
        self->buffer_index = !self->buffer_index;
        int16_t *buffer = (int16_t *)(void *)self->buffers[self->buffer_index];

        if (!mp3file_find_sync_word(self)) {
            return self->eof ? GET_BUFFER_DONE : GET_BUFFER_ERROR;
        }
        int bytes_left = BYTES_LEFT(self);
        uint8_t *inbuf = READ_PTR(self);
        int err = MP3Decode(self->decoder, &inbuf, &bytes_left, buffer, 0);
        CONSUME(self, BYTES_LEFT(self) - bytes_left);
        if (err) {
            return GET_BUFFER_DONE;
        }
    }

    return GET_BUFFER_MORE_DATA;
}

void audiomp3_mp3file_get_buffer_structure(audiomp3_mp3file_obj_t* self, bool single_channel,
                                           bool* single_buffer, bool* samples_signed,
                                           uint32_t* max_buffer_length, uint8_t* spacing) {
    *single_buffer = false;
    *samples_signed = true;
    *max_buffer_length = self->frame_buffer_size;
    if (single_channel) {
        *spacing = self->channel_count;
    } else {
        *spacing = 1;
    }
}