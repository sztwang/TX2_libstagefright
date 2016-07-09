/*
* Copyright (C) 2014 MediaTek Inc.
* Modification based on code covered by the mentioned copyright
* and/or permission notice(s).
*/
/*
 * Copyright (C) 2010 The Android Open Source Project
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

#ifndef MPEG2_TS_EXTRACTOR_H_

#define MPEG2_TS_EXTRACTOR_H_

#include <media/stagefright/foundation/ABase.h>
#include <media/stagefright/MediaExtractor.h>
#include <utils/threads.h>
#include <utils/Vector.h>

namespace android {

struct AMessage;
struct AnotherPacketSource;
struct ATSParser;
struct DataSource;
struct MPEG2TSSource;
struct String8;

struct MPEG2TSExtractor : public MediaExtractor {
    MPEG2TSExtractor(const sp<DataSource> &source);

    virtual size_t countTracks();
    virtual sp<MediaSource> getTrack(size_t index);
    virtual sp<MetaData> getTrackMetaData(size_t index, uint32_t flags);

    virtual sp<MetaData> getMetaData();

    virtual uint32_t flags() const;

#ifdef MTK_AOSP_ENHANCEMENT
    void seekTo(int64_t seekTimeUs);
    bool getSeeking();
    void setVideoState(bool state);
    bool getVideoState(void);
	bool consumeData(sp<AnotherPacketSource> impl,int64_t timeUS,bool isAudio);
	bool needRemoveData(sp<AnotherPacketSource> impl,int64_t timeUs,bool isAudio);
#endif
private:
    friend struct MPEG2TSSource;
#ifdef MTK_AOSP_ENHANCEMENT
    int64_t mDurationUs;
    int64_t mSeekTimeUs;
    bool mSeeking;
	bool End_OF_FILE;
    uint64_t mMaxcount;
    off64_t mSeekingOffset;
    off64_t mFileSize;
    off64_t mMinOffset;
    off64_t mMaxOffset;
    bool mVideoUnSupportedByDecoder;
#endif
    mutable Mutex mLock;

    sp<DataSource> mDataSource;

    sp<ATSParser> mParser;

    Vector<sp<AnotherPacketSource> > mSourceImpls;

	off64_t mOffsetPAT;

    off64_t mOffset;

    void init();
    status_t feedMore();

#ifdef MTK_AOSP_ENHANCEMENT
    status_t parseMaxPTS();
    uint64_t getDurationUs();
    bool findPAT();
#endif
    DISALLOW_EVIL_CONSTRUCTORS(MPEG2TSExtractor);
};

bool findSyncWord(const sp<DataSource> &source,off64_t StartOffset, uint64_t size, size_t PacketSize, off64_t &NewOffset);

bool SniffMPEG2TS(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *);

}  // namespace android

#endif  // MPEG2_TS_EXTRACTOR_H_
