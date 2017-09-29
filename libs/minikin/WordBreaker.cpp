/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Minikin"

#include <android/log.h>

#include <minikin/Emoji.h>
#include <minikin/Hyphenator.h>
#include "MinikinInternal.h"
#include "WordBreaker.h"
#include "cutils/log.h"

#include <unicode/uchar.h>
#include <unicode/utf16.h>

namespace minikin {

const uint32_t CHAR_SOFT_HYPHEN = 0x00AD;
const uint32_t CHAR_ZWJ = 0x200D;

std::unique_ptr<icu::BreakIterator> WordBreaker::createBreakIterator(const icu::Locale& locale) {
    // TODO: handle failure status
    UErrorCode status = U_ZERO_ERROR;
    return std::unique_ptr<icu::BreakIterator>(
            icu::BreakIterator::createLineInstance(locale, status));
}

ssize_t WordBreaker::followingWithLocale(const icu::Locale& locale, size_t from) {
    mBreakIterator = createBreakIterator(locale);
    UErrorCode status = U_ZERO_ERROR;
    LOG_ALWAYS_FATAL_IF(mText == nullptr, "setText must be called first");
    // TODO: handle failure status
    mBreakIterator->setText(&mUText, status);
    if (mInEmailOrUrl) {
        // Note:
        // Don't reset mCurrent, mLast, or mScanOffset for keeping email/URL context.
        // The email/URL detection doesn't support following() functionality, so that we can't
        // restart from the specific position. This means following() can not be supported in
        // general, but keeping old email/URL context works for LineBreaker since it just wants to
        // re-calculate the next break point with the new locale.
    } else {
        mCurrent = mLast = mScanOffset = from;
        next();
    }
    return mCurrent;
}

void WordBreaker::setText(const uint16_t* data, size_t size) {
    mText = data;
    mTextSize = size;
    mLast = 0;
    mCurrent = 0;
    mScanOffset = 0;
    mInEmailOrUrl = false;
    UErrorCode status = U_ZERO_ERROR;
    utext_openUChars(&mUText, reinterpret_cast<const UChar*>(data), size, &status);
}

ssize_t WordBreaker::current() const {
    return mCurrent;
}

/**
 * Determine whether a line break at position i within the buffer buf is valid. This
 * represents customization beyond the ICU behavior, because plain ICU provides some
 * line break opportunities that we don't want.
 **/
static bool isValidBreak(const uint16_t* buf, size_t bufEnd, int32_t i) {
    const size_t position = static_cast<size_t>(i);
    if (i == icu::BreakIterator::DONE || position == bufEnd) {
        // If the iterator reaches the end, treat as break.
        return true;
    }
    uint32_t codePoint;
    size_t prev_offset = position;
    U16_PREV(buf, 0, prev_offset, codePoint);
    // Do not break on hard or soft hyphens. These are handled by automatic hyphenation.
    if (Hyphenator::isLineBreakingHyphen(codePoint) || codePoint == CHAR_SOFT_HYPHEN) {
        return false;
    }
    // For Myanmar kinzi sequences, created by <consonant, ASAT, VIRAMA, consonant>. This is to go
    // around a bug in ICU line breaking: http://bugs.icu-project.org/trac/ticket/12561. To avoid
    // too much looking around in the strings, we simply avoid breaking after any Myanmar virama,
    // where no line break could be imagined, since the Myanmar virama is a pure stacker.
    if (codePoint == 0x1039) {  // MYANMAR SIGN VIRAMA
        return false;
    }

    uint32_t next_codepoint;
    size_t next_offset = position;
    U16_NEXT(buf, next_offset, bufEnd, next_codepoint);

    // Rule LB8 for Emoji ZWJ sequences. We need to do this ourselves since we may have fresher
    // emoji data than ICU does.
    if (codePoint == CHAR_ZWJ && isEmoji(next_codepoint)) {
        return false;
    }

    // Rule LB30b. We need to this ourselves since we may have fresher emoji data than ICU does.
    if (isEmojiModifier(next_codepoint)) {
        if (codePoint == 0xFE0F && prev_offset > 0) {
            // skip over emoji variation selector
            U16_PREV(buf, 0, prev_offset, codePoint);
        }
        if (isEmojiBase(codePoint)) {
            return false;
        }
    }
    return true;
}

// Customized iteratorNext that takes care of both resets and our modifications
// to ICU's behavior.
int32_t WordBreaker::iteratorNext() {
    int32_t result = mBreakIterator->following(mCurrent);
    while (!isValidBreak(mText, mTextSize, result)) {
        result = mBreakIterator->next();
    }
    return result;
}

// Chicago Manual of Style recommends breaking after these characters in URLs and email addresses
static bool breakAfter(uint16_t c) {
    return c == ':' || c == '=' || c == '&';
}

// Chicago Manual of Style recommends breaking before these characters in URLs and email addresses
static bool breakBefore(uint16_t c) {
    return c == '~' || c == '.' || c == ',' || c == '-' || c == '_' || c == '?' || c == '#'
            || c == '%' || c == '=' || c == '&';
}

enum ScanState {
    START,
    SAW_AT,
    SAW_COLON,
    SAW_COLON_SLASH,
    SAW_COLON_SLASH_SLASH,
};

void WordBreaker::detectEmailOrUrl() {
    // scan forward from current ICU position for email address or URL
    if (mLast >= mScanOffset) {
        ScanState state = START;
        size_t i;
        for (i = mLast; i < mTextSize; i++) {
            uint16_t c = mText[i];
            // scan only ASCII characters, stop at space
            if (!(' ' < c && c <= 0x007E)) {
                break;
            }
            if (state == START && c == '@') {
                state = SAW_AT;
            } else if (state == START && c == ':') {
                state = SAW_COLON;
            } else if (state == SAW_COLON || state == SAW_COLON_SLASH) {
                if (c == '/') {
                    state = static_cast<ScanState>((int)state + 1);  // next state adds a slash
                } else {
                    state = START;
                }
            }
        }
        if (state == SAW_AT || state == SAW_COLON_SLASH_SLASH) {
            if (!mBreakIterator->isBoundary(i)) {
                // If there are combining marks or such at the end of the URL or the email address,
                // consider them a part of the URL or the email, and skip to the next actual
                // boundary.
                i = mBreakIterator->following(i);
            }
            mInEmailOrUrl = true;
        } else {
            mInEmailOrUrl = false;
        }
        mScanOffset = i;
    }
}

ssize_t WordBreaker::findNextBreakInEmailOrUrl() {
    // special rules for email addresses and URL's as per Chicago Manual of Style (16th ed.)
    uint16_t lastChar = mText[mLast];
    ssize_t i;
    for (i = mLast + 1; i < mScanOffset; i++) {
        if (breakAfter(lastChar)) {
            break;
        }
        // break after double slash
        if (lastChar == '/' && i >= mLast + 2 && mText[i - 2] == '/') {
            break;
        }
        const uint16_t thisChar = mText[i];
        // never break after hyphen
        if (lastChar != '-') {
            if (breakBefore(thisChar)) {
                break;
            }
            // break before single slash
            if (thisChar == '/' && lastChar != '/' &&
                        !(i + 1 < mScanOffset && mText[i + 1] == '/')) {
                break;
            }
        }
        lastChar = thisChar;
    }
    return i;
}

ssize_t WordBreaker::next() {
    mLast = mCurrent;

    detectEmailOrUrl();
    if (mInEmailOrUrl) {
        mCurrent = findNextBreakInEmailOrUrl();
    } else {  // Business as usual
        mCurrent = (ssize_t) iteratorNext();
    }
    return mCurrent;
}

ssize_t WordBreaker::wordStart() const {
    if (mInEmailOrUrl) {
        return mLast;
    }
    ssize_t result = mLast;
    while (result < mCurrent) {
        UChar32 c;
        ssize_t ix = result;
        U16_NEXT(mText, ix, mCurrent, c);
        const int32_t lb = u_getIntPropertyValue(c, UCHAR_LINE_BREAK);
        // strip leading punctuation, defined as OP and QU line breaking classes,
        // see UAX #14
        if (!(lb == U_LB_OPEN_PUNCTUATION || lb == U_LB_QUOTATION)) {
            break;
        }
        result = ix;
    }
    return result;
}

ssize_t WordBreaker::wordEnd() const {
    if (mInEmailOrUrl) {
        return mLast;
    }
    ssize_t result = mCurrent;
    while (result > mLast) {
        UChar32 c;
        ssize_t ix = result;
        U16_PREV(mText, mLast, ix, c);
        const int32_t gc_mask = U_GET_GC_MASK(c);
        // strip trailing space and punctuation
        if ((gc_mask & (U_GC_ZS_MASK | U_GC_P_MASK)) == 0) {
            break;
        }
        result = ix;
    }
    return result;
}

int WordBreaker::breakBadness() const {
    return (mInEmailOrUrl && mCurrent < mScanOffset) ? 1 : 0;
}

void WordBreaker::finish() {
    mText = nullptr;
    // Note: calling utext_close multiply is safe
    utext_close(&mUText);
}

}  // namespace minikin
