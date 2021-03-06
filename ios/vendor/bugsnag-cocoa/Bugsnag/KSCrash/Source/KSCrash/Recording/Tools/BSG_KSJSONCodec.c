//
// BSG_KSJSONCodec.c
//
//  Created by Karl Stenerud on 2012-01-07.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

#include "BSG_KSJSONCodec.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ============================================================================
#pragma mark - Configuration -
// ============================================================================

/** Set to 1 if you're also compiling BSG_KSLogger and want to use it here */
#ifndef BSG_KSJSONCODEC_UseKSLogger
#define BSG_KSJSONCODEC_UseKSLogger 1
#endif

#if BSG_KSJSONCODEC_UseKSLogger
#include "BSG_KSLogger.h"
#else
#define BSG_KSLOG_ERROR(FMT, ...)
#endif


/** The work buffer size to use when escaping string values.
 * There's little reason to change this since nothing ever gets truncated.
 */
#ifndef BSG_KSJSONCODEC_WorkBufferSize
#define BSG_KSJSONCODEC_WorkBufferSize 512
#endif

/**
 * The maximum number of significant digits when printing floats.
 * 7 (6 + 1 whole digit in exp form) is the default used by the old sprintf code.
 */
#define MAX_SIGNIFICANT_DIGITS 7

// ============================================================================
#pragma mark - Helpers -
// ============================================================================

// Compiler hints for "if" statements
#define likely_if(x) if (__builtin_expect(x, 1))
#define unlikely_if(x) if (__builtin_expect(x, 0))

/** Used for writing hex string values. */
static char bsg_g_hexNybbles[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

const char *bsg_ksjsonstringForError(const int error) {
    switch (error) {
    case BSG_KSJSON_ERROR_INVALID_CHARACTER:
        return "Invalid character";
    case BSG_KSJSON_ERROR_CANNOT_ADD_DATA:
        return "Cannot add data";
    case BSG_KSJSON_ERROR_INCOMPLETE:
        return "Incomplete data";
    case BSG_KSJSON_ERROR_INVALID_DATA:
        return "Invalid data";
    default:
        return "(unknown error)";
    }
}

// Max uint64 is 18446744073709551615
#define MAX_UINT64_DIGITS 20

/**
 * Convert an unsigned integer to a string.
 * This will write a maximum of 21 characters (including the NUL) to dst.
 *
 * Returns the length of the string written to dst (not including the NUL).
 */
static size_t uint64_to_string(uint64_t value, char* dst) {
    if(value == 0) {
        dst[0] = '0';
        dst[1] = 0;
        return 1;
    }

    char buff[MAX_UINT64_DIGITS+1];
    buff[sizeof(buff)-1] = 0;
    int index = sizeof(buff) - 2;
    for(;;) {
        buff[index] = (value%10) + '0';
        value /= 10;
        if (value == 0) {
            break;
        }
        index--;
    }

    size_t length = sizeof(buff) - index;
    memcpy(dst, buff+index, length);
    return length - 1;
}

/**
 * Convert an integer to a string.
 * This will write a maximum of 22 characters (including the null terminator) to dst.
 *
 * Returns the length of the string written to dst (not including the null termination byte).
 */
static size_t int64_to_string(int64_t value, char* dst) {
    if (value < 0) {
        dst[0] = '-';
        return uint64_to_string(-value, dst+1) + 1;
    }
    return uint64_to_string(value, dst);
}

/**
 * Convert a positive double to a string, allowing up to max_sig_digits.
 * To reduce the complexity of this algorithm, values with an exponent
 * other than 0 are always printed in exponential form.
 *
 * Values are rounded half-up.
 *
 * This function makes use of compiler intrinsic functions which, though not
 * officially async-safe, are actually async-safe (no allocations, locks, etc).
 *
 * This function will write a maximum of 21 characters (including the NUL) to dst.
 *
 * Returns the length of the string written to dst (not including the NUL).
 */
static size_t positive_double_to_string(const double value, char* dst, const int max_sig_digits) {
    const char* const orig_dst = dst;

    if(value == 0) {
        dst[0] = '0';
        dst[1] = 0;
        return 1;
    }

    // isnan() is basically ((x) != (x))
    if(isnan(value)) {
        strcpy(dst, "nan");
        return 3;
    }

    // isinf() is a compiler intrinsic.
    if(isinf(value)) {
        strcpy(dst, "inf");
        return 3;
    }

    // log10() is a compiler intrinsic.
    int exponent = log10(value);
    // Values < 1.0 must subtract 1 from exponent to handle zero wraparound.
    if (value < 1.0) {
        exponent--;
    }

    // pow() is a compiler intrinsic.
    double normalized = value / pow(10, exponent);
    // Special case for 0.1, 0.01, 0.001, etc giving a normalized value of 10.xyz.
    // We use 9.999... because 10.0 converts to a value > 10 in ieee754 binary floats.
    if (normalized > 9.99999999999999822364316059975) {
        exponent++;
        normalized = value / pow(10, exponent);
    }

    // Put all of the digits we'll use into an integer.
    double digits_and_remainder = normalized * pow(10, max_sig_digits-1);
    uint64_t digits = digits_and_remainder;
    // Also round up if necessary (note: 0.5 is exact in both binary and decimal).
    if (digits_and_remainder - digits >= 0.5) {
        digits++;
        // Special case: Adding one bumps us to next magnitude.
        if (digits >= (uint64_t)pow(10, max_sig_digits)) {
            exponent++;
            digits /= 10;
        }
    }

    // Extract the fractional digits.
    for (int i = max_sig_digits; i > 1; i--) {
        dst[i] = digits % 10 + '0';
        digits /= 10;
    }
    // Extract the single-digit whole part.
    dst[0] = digits + '0';
    dst[1] = '.';

    // Strip off trailing zeroes, and also the '.' if there is no fractional part.
    int e_offset = max_sig_digits;
    for (int i = max_sig_digits; i > 0; i--) {
        if (dst[i] != '0') {
            if (dst[i] == '.') {
                e_offset = i;
            } else {
                e_offset = i + 1;
            }
            break;
        }
    }
    dst += e_offset;

    // Add the exponent if it's not 0.
    if (exponent != 0) {
        *dst++ = 'e';
        if (exponent >= 0) {
            *dst++ = '+';
        }
        dst += int64_to_string(exponent, dst);
    } else {
        *dst = 0;
    }

    return dst - orig_dst;
}

/**
 * Convert a positive double to a string, allowing up to max_sig_digits. See
 * positive_double_to_string() for important information about how this
 * algorithm differs from sprintf.
 *
 * This function will write a maximum of 22 characters (including the NUL) to dst.
 *
 * Returns the length of the string written to dst (not including the NUL).
 */
static size_t double_to_string(double value, char* dst, int max_sig_digits) {
    if (value < 0) {
        dst[0] = '-';
        return positive_double_to_string(-value, dst+1, max_sig_digits) + 1;
    }
    return positive_double_to_string(value, dst, max_sig_digits);
}

// ============================================================================
#pragma mark - Encode -
// ============================================================================

// Avoiding static functions due to linker issues.

/** Add JSON encoded data to an external handler.
 * The external handler will decide how to handle the data (store/transmit/etc).
 *
 * @param context The encoding context.
 *
 * @param data The encoded data.
 *
 * @param length The length of the data.
 *
 * @return true if the data was handled successfully.
 */
#define addJSONData(CONTEXT, DATA, LENGTH)                                     \
    (CONTEXT)->addJSONData(DATA, LENGTH, (CONTEXT)->userData)

/** Escape a string portion for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return true if the data was handled successfully.
 */
int bsg_ksjsoncodec_i_appendEscapedString(
    BSG_KSJSONEncodeContext *const context, const char *restrict const string,
    size_t length) {
    char workBuffer[BSG_KSJSONCODEC_WorkBufferSize];
    const char *const srcEnd = string + length;

    const char *restrict src = string;
    char *restrict dst = workBuffer;

    // Simple case (no escape or special characters)
    for (; src < srcEnd && *src != '\\' && *src != '\"' &&
           (unsigned char)*src >= ' ';
         src++) {
        *dst++ = *src;
    }

    // Deal with complicated case (if any)
    int result;
    for (; src < srcEnd; src++) {
        
        // If we add an escaped control character this may exceed the buffer by up to
        // 6 characters: add this chunk now, reset the buffer and carry on
        if (dst + 6 > workBuffer + BSG_KSJSONCODEC_WorkBufferSize) {
            size_t encLength = (size_t)(dst - workBuffer);
            unlikely_if((result = addJSONData(context, dst - encLength, encLength)) !=
                        BSG_KSJSON_OK) {
                return result;
            }
            dst = workBuffer;
        }
        
        switch (*src) {
        case '\\':
        case '\"':
            *dst++ = '\\';
            *dst++ = *src;
            break;
        case '\b':
            *dst++ = '\\';
            *dst++ = 'b';
            break;
        case '\f':
            *dst++ = '\\';
            *dst++ = 'f';
            break;
        case '\n':
            *dst++ = '\\';
            *dst++ = 'n';
            break;
        case '\r':
            *dst++ = '\\';
            *dst++ = 'r';
            break;
        case '\t':
            *dst++ = '\\';
            *dst++ = 't';
            break;
        default:

            // escape control chars (U+0000 - U+001F)
            // see https://www.ietf.org/rfc/rfc4627.txt

            if ((unsigned char)*src < ' ') {
                unsigned int last = *src % 16;
                unsigned int first = (*src - last) / 16;

                *dst++ = '\\';
                *dst++ = 'u';
                *dst++ = '0';
                *dst++ = '0';
                *dst++ = bsg_g_hexNybbles[first];
                *dst++ = bsg_g_hexNybbles[last];
            } else {
                *dst++ = *src;
            }
            break;
        }
    }
    size_t encLength = (size_t)(dst - workBuffer);
    return addJSONData(context, dst - encLength, encLength);
}

/** Escape a string for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return true if the data was handled successfully.
 */
int bsg_ksjsoncodec_i_addEscapedString(BSG_KSJSONEncodeContext *const context,
                                       const char *restrict const string,
                                       size_t length) {
    int result = BSG_KSJSON_OK;

    // Keep adding portions until the whole string has been processed.
    size_t offset = 0;
    while (offset < length) {
        size_t toAdd = length - offset;
        unlikely_if(toAdd > BSG_KSJSONCODEC_WorkBufferSize) {
            toAdd = BSG_KSJSONCODEC_WorkBufferSize;
        }
        result = bsg_ksjsoncodec_i_appendEscapedString(context, string + offset,
                                                       toAdd);
        unlikely_if(result != BSG_KSJSON_OK) { break; }
        offset += toAdd;
    }
    return result;
}

/** Escape and quote a string for use with JSON and send to data handler.
 *
 * @param context The JSON context.
 *
 * @param string The string to escape and write.
 *
 * @param length The length of the string.
 *
 * @return true if the data was handled successfully.
 */
int bsg_ksjsoncodec_i_addQuotedEscapedString(
    BSG_KSJSONEncodeContext *const context, const char *restrict const string,
    size_t length) {
    int result;
    unlikely_if((result = addJSONData(context, "\"", 1)) != BSG_KSJSON_OK) {
        return result;
    }
    unlikely_if((result = bsg_ksjsoncodec_i_addEscapedString(
                     context, string, length)) != BSG_KSJSON_OK) {
        return result;
    }
    return addJSONData(context, "\"", 1);
}

int bsg_ksjsonbeginElement(BSG_KSJSONEncodeContext *const context,
                           const char *const name) {
    int result = BSG_KSJSON_OK;

    // Decide if a comma is warranted.
    unlikely_if(context->containerFirstEntry) {
        context->containerFirstEntry = false;
    }
    else {
        unlikely_if((result = addJSONData(context, ",", 1)) != BSG_KSJSON_OK) {
            return result;
        }
    }

    // Pretty printing
    unlikely_if(context->prettyPrint && context->containerLevel > 0) {
        unlikely_if((result = addJSONData(context, "\n", 1)) != BSG_KSJSON_OK) {
            return result;
        }
        for (int i = 0; i < context->containerLevel; i++) {
            unlikely_if((result = addJSONData(context, "    ", 4)) !=
                        BSG_KSJSON_OK) {
                return result;
            }
        }
    }

    // Add a name field if we're in an object.
    if (context->isObject[context->containerLevel]) {
        unlikely_if(name == NULL) {
            BSG_KSLOG_ERROR("Name was null inside an object");
            return BSG_KSJSON_ERROR_INVALID_DATA;
        }
        unlikely_if((result = bsg_ksjsoncodec_i_addQuotedEscapedString(
                         context, name, strlen(name))) != BSG_KSJSON_OK) {
            return result;
        }
        unlikely_if(context->prettyPrint) {
            unlikely_if((result = addJSONData(context, ": ", 2)) !=
                        BSG_KSJSON_OK) {
                return result;
            }
        }
        else {
            unlikely_if((result = addJSONData(context, ":", 1)) !=
                        BSG_KSJSON_OK) {
                return result;
            }
        }
    }
    return result;
}

int bsg_ksjsonaddRawJSONData(BSG_KSJSONEncodeContext *const context,
                             const char *const data, const size_t length) {
    return addJSONData(context, data, length);
}

int bsg_ksjsonaddBooleanElement(BSG_KSJSONEncodeContext *const context,
                                const char *const name, const bool value) {
    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    if (value) {
        return addJSONData(context, "true", 4);
    } else {
        return addJSONData(context, "false", 5);
    }
}

int bsg_ksjsonaddFloatingPointElement(BSG_KSJSONEncodeContext *const context,
                                      const char *const name, double value) {
    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    char buff[30];
    double_to_string(value, buff, MAX_SIGNIFICANT_DIGITS);
    return addJSONData(context, buff, strlen(buff));
}

int bsg_ksjsonaddIntegerElement(BSG_KSJSONEncodeContext *const context,
                                const char *const name, long long value) {
    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    char buff[30];
    int64_to_string(value, buff);
    return addJSONData(context, buff, strlen(buff));
}

int bsg_ksjsonaddUIntegerElement(BSG_KSJSONEncodeContext *const context,
                                 const char *const name,
                                 unsigned long long value) {
    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    char buff[30];
    uint64_to_string(value, buff);
    return addJSONData(context, buff, (int)strlen(buff));
}

int bsg_ksjsonaddJSONElement(BSG_KSJSONEncodeContext *const context,
                             const char *restrict const name,
                             const char *restrict const element,
                             size_t length) {
    unlikely_if(element == NULL) {
        return bsg_ksjsonaddNullElement(context, name);
    }
    size_t idx = 0;
    while (idx < length && (element[idx] == ' ' || element[idx] == '\r' ||
                            element[idx] == '\n' || element[idx] == '\t' ||
                            element[idx] == '\f')) {
        idx++;
    }
    unlikely_if(idx >= length) {
        BSG_KSLOG_ERROR("JSON element contained no JSON data: %s", element);
        return BSG_KSJSON_ERROR_INVALID_DATA;
    }
    switch (element[idx]) {
    case '[':
    case '{':
    case '\"':
    case 'f':
    case 't':
    case 'n':
    case '-':
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        break;
    default:
        BSG_KSLOG_ERROR("Invalid character '%c' in: ", element[idx], element);
        return BSG_KSJSON_ERROR_INVALID_DATA;
    }

    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    return addJSONData(context, element, length);
}

int bsg_ksjsonaddNullElement(BSG_KSJSONEncodeContext *const context,
                             const char *const name) {
    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    return addJSONData(context, "null", 4);
}

int bsg_ksjsonaddStringElement(BSG_KSJSONEncodeContext *const context,
                               const char *const name, const char *const value,
                               size_t length) {
    unlikely_if(value == NULL) {
        return bsg_ksjsonaddNullElement(context, name);
    }
    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    if (length == BSG_KSJSON_SIZE_AUTOMATIC) {
        length = strlen(value);
    }
    return bsg_ksjsoncodec_i_addQuotedEscapedString(context, value, length);
}

int bsg_ksjsonbeginStringElement(BSG_KSJSONEncodeContext *const context,
                                 const char *const name) {
    int result = bsg_ksjsonbeginElement(context, name);
    unlikely_if(result != BSG_KSJSON_OK) { return result; }
    return addJSONData(context, "\"", 1);
}

int bsg_ksjsonappendStringElement(BSG_KSJSONEncodeContext *const context,
                                  const char *const value, size_t length) {
    return bsg_ksjsoncodec_i_addEscapedString(context, value, length);
}

int bsg_ksjsonendStringElement(BSG_KSJSONEncodeContext *const context) {
    return addJSONData(context, "\"", 1);
}

int bsg_ksjsonaddDataElement(BSG_KSJSONEncodeContext *const context,
                             const char *name, const char *value,
                             size_t length) {
    int result = BSG_KSJSON_OK;
    result = bsg_ksjsonbeginDataElement(context, name);
    if (result == BSG_KSJSON_OK) {
        result = bsg_ksjsonappendDataElement(context, value, length);
    }
    if (result == BSG_KSJSON_OK) {
        result = bsg_ksjsonendDataElement(context);
    }
    return result;
}

int bsg_ksjsonbeginDataElement(BSG_KSJSONEncodeContext *const context,
                               const char *const name) {
    return bsg_ksjsonbeginStringElement(context, name);
}

int bsg_ksjsonappendDataElement(BSG_KSJSONEncodeContext *const context,
                                const char *const value, size_t length) {
    unsigned char *currentByte = (unsigned char *)value;
    unsigned char *end = currentByte + length;
    char chars[2];
    int result = BSG_KSJSON_OK;
    while (currentByte < end) {
        chars[0] = bsg_g_hexNybbles[(*currentByte >> 4) & 15];
        chars[1] = bsg_g_hexNybbles[*currentByte & 15];
        result = addJSONData(context, chars, sizeof(chars));
        if (result != BSG_KSJSON_OK) {
            break;
        }
        currentByte++;
    }
    return result;
}

int bsg_ksjsonendDataElement(BSG_KSJSONEncodeContext *const context) {
    return bsg_ksjsonendStringElement(context);
}

int bsg_ksjsonbeginArray(BSG_KSJSONEncodeContext *const context,
                         const char *const name) {
    likely_if(context->containerLevel >= 0) {
        int result = bsg_ksjsonbeginElement(context, name);
        unlikely_if(result != BSG_KSJSON_OK) { return result; }
    }

    context->containerLevel++;
    context->isObject[context->containerLevel] = false;
    context->containerFirstEntry = true;

    return addJSONData(context, "[", 1);
}

int bsg_ksjsonbeginObject(BSG_KSJSONEncodeContext *const context,
                          const char *const name) {
    likely_if(context->containerLevel >= 0) {
        int result = bsg_ksjsonbeginElement(context, name);
        unlikely_if(result != BSG_KSJSON_OK) { return result; }
    }

    context->containerLevel++;
    context->isObject[context->containerLevel] = true;
    context->containerFirstEntry = true;

    return addJSONData(context, "{", 1);
}

int bsg_ksjsonendContainer(BSG_KSJSONEncodeContext *const context) {
    unlikely_if(context->containerLevel <= 0) { return BSG_KSJSON_OK; }

    bool isObject = context->isObject[context->containerLevel];
    context->containerLevel--;

    // Pretty printing
    unlikely_if(context->prettyPrint && !context->containerFirstEntry) {
        int result;
        unlikely_if((result = addJSONData(context, "\n", 1)) != BSG_KSJSON_OK) {
            return result;
        }
        for (int i = 0; i < context->containerLevel; i++) {
            unlikely_if((result = addJSONData(context, "    ", 4)) !=
                        BSG_KSJSON_OK) {
                return result;
            }
        }
    }
    context->containerFirstEntry = false;
    return addJSONData(context, isObject ? "}" : "]", 1);
}

void bsg_ksjsonbeginEncode(BSG_KSJSONEncodeContext *const context,
                           bool prettyPrint,
                           BSG_KSJSONAddDataFunc addJSONDataFunc,
                           void *const userData) {
    memset(context, 0, sizeof(*context));
    context->addJSONData = addJSONDataFunc;
    context->userData = userData;
    context->prettyPrint = prettyPrint;
    context->containerFirstEntry = true;
}

int bsg_ksjsonendEncode(BSG_KSJSONEncodeContext *const context) {
    int result = BSG_KSJSON_OK;
    while (context->containerLevel > 0) {
        unlikely_if((result = bsg_ksjsonendContainer(context)) !=
                    BSG_KSJSON_OK) {
            return result;
        }
    }
    return result;
}
