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

//#define LOG_NDEBUG 0
#define LOG_TAG "OggExtractor"
#include <utils/Log.h>

#include "include/OggExtractor.h"

#include <cutils/properties.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/base64.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaBufferGroup.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>
#include <utils/String8.h>
#ifdef MTK_AOSP_ENHANCEMENT
#include <cutils/xlog.h>
#endif

extern "C" {
    #include <Tremolo/codec_internal.h>

    int _vorbis_unpack_books(vorbis_info *vi,oggpack_buffer *opb);
    int _vorbis_unpack_info(vorbis_info *vi,oggpack_buffer *opb);
    int _vorbis_unpack_comment(vorbis_comment *vc,oggpack_buffer *opb);
    long vorbis_packet_blocksize(vorbis_info *vi,ogg_packet *op);
}

namespace android {

struct OggSource : public MediaSource {
    OggSource(const sp<OggExtractor> &extractor);

    virtual sp<MetaData> getFormat();

    virtual status_t start(MetaData *params = NULL);
    virtual status_t stop();

    virtual status_t read(
            MediaBuffer **buffer, const ReadOptions *options = NULL);

protected:
    virtual ~OggSource();

private:
    sp<OggExtractor> mExtractor;
    bool mStarted;

    OggSource(const OggSource &);
    OggSource &operator=(const OggSource &);
};

struct MyVorbisExtractor {
    MyVorbisExtractor(const sp<DataSource> &source);
    virtual ~MyVorbisExtractor();

    sp<MetaData> getFormat() const;

    // Returns an approximate bitrate in bits per second.
    uint64_t approxBitrate();

    status_t seekToTime(int64_t timeUs);
    status_t seekToOffset(off64_t offset);
    status_t readNextPacket(MediaBuffer **buffer, bool conf);

    status_t init();
#ifdef MTK_AOSP_ENHANCEMENT
    void startTocThread()
    {
        tocThread = new TocThread(this);
        if(tocThread.get())
          tocThread->run("VorbisTocThread");
    }
    void stopTocThread()
    {
        if(tocThread.get())
          if((tocThread->requestExitAndWait()) == WOULD_BLOCK)
            tocThread->requestExit();
    }
    void setState(bool state){mTocStarted = state;}
    bool isCachingDataSource()
    {
        return mSource->flags() & DataSource::kIsCachingDataSource;
    }
#endif

    sp<MetaData> getFileMetaData() { return mFileMeta; }

private:
    struct Page {
        uint64_t mGranulePosition;
        int32_t mPrevPacketSize;
        uint64_t mPrevPacketPos;
        uint32_t mSerialNo;
        uint32_t mPageNo;
        uint8_t mFlags;
        uint8_t mNumSegments;
        uint8_t mLace[255];
    };

    struct TOCEntry {
        off64_t mPageOffset;
        int64_t mTimeUs;
    };

    sp<DataSource> mSource;
    off64_t mOffset;
    Page mCurrentPage;
    uint64_t mPrevGranulePosition;
    size_t mCurrentPageSize;
    bool mFirstPacketInPage;
    uint64_t mCurrentPageSamples;
    size_t mNextLaceIndex;

    off64_t mFirstDataOffset;
#ifdef MTK_AOSP_ENHANCEMENT
    class TocThread:public Thread
    {
        friend class MyVorbisExtractor;
        public:
           TocThread(MyVorbisExtractor * vorExtractor):m_vorExtractor(vorExtractor){}
        private:
            virtual bool threadLoop()
            {
               
               m_vorExtractor->buildTableOfContents();
               return false;//let toc building only once
            }
            MyVorbisExtractor * m_vorExtractor;
    };
    //find page offset via samples in range(ofStart,ofEnd)
    off64_t findAccuratePageOffset(uint64_t samples,off64_t ofStart,off64_t ofEnd);
    //find granuleposition of the page via offset
    status_t findGranulePositionofPage(off64_t offset,uint64_t *granulePositionofPage);
    //find next page from startOffset
    status_t findNextPage_l(off64_t startOffset, off64_t *pageOffset);
    
    bool mTocStarted;
    bool mTocDone;
    uint64_t mFileSize;
    sp<TocThread> tocThread;
#endif

    vorbis_info mVi;
    vorbis_comment mVc;

    sp<MetaData> mMeta;
    sp<MetaData> mFileMeta;

    Vector<TOCEntry> mTableOfContents;

    ssize_t readPage(off64_t offset, Page *page);
    status_t findNextPage(off64_t startOffset, off64_t *pageOffset);

    status_t verifyHeader(
            MediaBuffer *buffer, uint8_t type);

    int32_t packetBlockSize(MediaBuffer *buffer);

    void parseFileMetaData();

    status_t findPrevGranulePosition(off64_t pageOffset, uint64_t *granulePos);

    void buildTableOfContents();

    MyVorbisExtractor(const MyVorbisExtractor &);
    MyVorbisExtractor &operator=(const MyVorbisExtractor &);
};

static void extractAlbumArt(
        const sp<MetaData> &fileMeta, const void *data, size_t size);

////////////////////////////////////////////////////////////////////////////////

OggSource::OggSource(const sp<OggExtractor> &extractor)
    : mExtractor(extractor),
      mStarted(false) {
}

OggSource::~OggSource() {
    if (mStarted) {
        stop();
    }
}

sp<MetaData> OggSource::getFormat() {
    return mExtractor->mImpl->getFormat();
}

status_t OggSource::start(MetaData * /* params */) {
    if (mStarted) {
        return INVALID_OPERATION;
    }

    mStarted = true;
#ifdef MTK_AOSP_ENHANCEMENT
    //start TOC thread for local playback
    if (!(mExtractor->mImpl->isCachingDataSource()))
    {
       mExtractor->mImpl->setState(true);
       mExtractor->mImpl->startTocThread();
    }
#endif

    return OK;
}

status_t OggSource::stop() {
    mStarted = false;
#ifdef MTK_AOSP_ENHANCEMENT   
    //stop TOC thread for local playback
    if (!(mExtractor->mImpl->isCachingDataSource()))
    {
       mExtractor->mImpl->setState(false);
       mExtractor->mImpl->stopTocThread();
    }
#endif

    return OK;
}

status_t OggSource::read(
        MediaBuffer **out, const ReadOptions *options) {
    *out = NULL;

    int64_t seekTimeUs;
    ReadOptions::SeekMode mode;
#ifdef MTK_AOSP_ENHANCEMENT  
    bool isSeeking = false;
#endif    	
    if (options && options->getSeekTo(&seekTimeUs, &mode)) {
        if (mExtractor->mImpl->seekToTime(seekTimeUs) != OK) {
            return ERROR_END_OF_STREAM;
        }
#ifdef MTK_AOSP_ENHANCEMENT
        isSeeking = true;
#endif
    }

    MediaBuffer *packet;
    status_t err = mExtractor->mImpl->readNextPacket(&packet, /* conf = */ false);

    if (err != OK) {
#ifdef MTK_AOSP_ENHANCEMENT
        if(err == ERROR_MALFORMED)
            return ERROR_END_OF_STREAM;
#endif
        return err;
    }

#if 0
    int64_t timeUs;
    if (packet->meta_data()->findInt64(kKeyTime, &timeUs)) {
        ALOGI("found time = %lld us", timeUs);
    } else {
        ALOGI("NO time");
    }
#endif
#ifdef MTK_AOSP_ENHANCEMENT
   if(isSeeking && (seekTimeUs > 0))
   {
   //Discard first packet while seek(avoid noise)
    ssize_t nlength = packet->range_length();
    SXLOGD("Read :%x bytes and discard",nlength);
    packet->release();
    packet = NULL;
    
    err = mExtractor->mImpl->readNextPacket(&packet, /* conf = */ false);
    if (err != OK) {
        if(err == ERROR_MALFORMED)
            return ERROR_END_OF_STREAM;
        return err;
    }
    nlength = packet->range_length();
#if 0
    SXLOGD("Read length:%x bytes",nlength);

    FILE *fp_in = fopen("/sdcard/ReadPacket_seek.pcm", "ab");
    if(fp_in)
     {
      fprintf(fp_in, "vPkT");
      fwrite(&nlength, 1, 4, fp_in);
      fwrite(packet->data(), 1, nlength, fp_in);
      fclose(fp_in);
     }
     
    int64_t timeUs;
    if (packet->meta_data()->findInt64(kKeyTime, &timeUs)) {
        SXLOGD("found time = %lld us", timeUs);
    } else {
        SXLOGD("NO time");
    }
#endif
    isSeeking = false;
    }
#endif

    packet->meta_data()->setInt32(kKeyIsSyncFrame, 1);

    *out = packet;

    return OK;
}

////////////////////////////////////////////////////////////////////////////////

MyVorbisExtractor::MyVorbisExtractor(const sp<DataSource> &source)
    : mSource(source),
      mOffset(0),
      mPrevGranulePosition(0),
      mCurrentPageSize(0),
      mFirstPacketInPage(true),
      mCurrentPageSamples(0),
      mNextLaceIndex(0),
      mFirstDataOffset(-1) {
    mCurrentPage.mNumSegments = 0;
#ifdef MTK_AOSP_ENHANCEMENT
    mTocStarted = false;
    mTocDone = false;
    mFileSize = 0;
#endif
    vorbis_info_init(&mVi);
    vorbis_comment_init(&mVc);
}

MyVorbisExtractor::~MyVorbisExtractor() {
    vorbis_comment_clear(&mVc);
    vorbis_info_clear(&mVi);
}

sp<MetaData> MyVorbisExtractor::getFormat() const {
    return mMeta;
}

status_t MyVorbisExtractor::findNextPage(
        off64_t startOffset, off64_t *pageOffset) {
    *pageOffset = startOffset;
    
#ifdef MTK_AOSP_ENHANCEMENT  
    //optimize find page process   
    char signature[2048];

    for (;;) {
      ssize_t n = mSource->readAt(*pageOffset, &signature, 2000);

        if (n < 4) {
            *pageOffset = 0;

            return (n < 0) ? n : (status_t)ERROR_END_OF_STREAM;
        }
        int i = 0;
        int step = n-4;
        while(i<=step)
        {
            if (!memcmp(&signature[i], "OggS", 4)) {
                if ((*pageOffset + i) > startOffset) {
                    SXLOGV("skipped %lld bytes of junk to reach next frame",
                         (*pageOffset + i) - startOffset);
                }
                *pageOffset += i;
                return OK;
            }
            
            i++;
         }
        *pageOffset += n;
      
    }
#else
    for (;;) {
        char signature[4];
        ssize_t n = mSource->readAt(*pageOffset, &signature, 4);

        if (n < 4) {
            *pageOffset = 0;

            return (n < 0) ? n : (status_t)ERROR_END_OF_STREAM;
        }

        if (!memcmp(signature, "OggS", 4)) {
            if (*pageOffset > startOffset) {
                ALOGV("skipped %lld bytes of junk to reach next frame",
                     *pageOffset - startOffset);
            }

            return OK;
        }

        ++*pageOffset;
    }
#endif                                                            
}

// Given the offset of the "current" page, find the page immediately preceding
// it (if any) and return its granule position.
// To do this we back up from the "current" page's offset until we find any
// page preceding it and then scan forward to just before the current page.
status_t MyVorbisExtractor::findPrevGranulePosition(
        off64_t pageOffset, uint64_t *granulePos) {
    *granulePos = 0;

    off64_t prevPageOffset = 0;
    off64_t prevGuess = pageOffset;
    for (;;) {
        if (prevGuess >= 5000) {
            prevGuess -= 5000;
        } else {
            prevGuess = 0;
        }

        ALOGV("backing up %lld bytes", pageOffset - prevGuess);

        status_t err = findNextPage(prevGuess, &prevPageOffset);
        if (err != OK) {
#ifdef MTK_AOSP_ENHANCEMENT   
            if((prevGuess == 0) || ((prevGuess + 5000 * 20) < pageOffset))
                return err;
            else
                continue;
#else
            return err;
#endif            
        }

        if (prevPageOffset < pageOffset || prevGuess == 0) {
            break;
        }
    }

    if (prevPageOffset == pageOffset) {
        // We did not find a page preceding this one.
        return UNKNOWN_ERROR;
    }

    ALOGV("prevPageOffset at %lld, pageOffset at %lld",
         prevPageOffset, pageOffset);

    for (;;) {
        Page prevPage;
        ssize_t n = readPage(prevPageOffset, &prevPage);

        if (n <= 0) {
            return (status_t)n;
        }

        prevPageOffset += n;

        if (prevPageOffset == pageOffset) {
            *granulePos = prevPage.mGranulePosition;
            return OK;
        }
    }
}

status_t MyVorbisExtractor::seekToTime(int64_t timeUs) {
#ifdef MTK_AOSP_ENHANCEMENT
    if(timeUs == 0)
        return seekToOffset(0);
    if (isCachingDataSource())
    {
        off64_t pos = timeUs * approxBitrate() / 8000000ll;
        SXLOGV("seeking to offset %lld", pos);
        return seekToOffset(pos);
    }
    if (mTableOfContents.isEmpty() || (mTocDone == false)) {
    	  // Perform seekto accurate page.
    	  uint64_t ts1,ts2;
    	  if(findGranulePositionofPage(mFirstDataOffset,&ts1) != OK)
    	  	return seekToOffset(0);
    	  if(findGranulePositionofPage(mFileSize,&ts2) != OK)
    	  	return seekToOffset(0);
    	  SXLOGD("bitrate seek--pos:%lld,ts1:%lld,ts2:%lld",timeUs * mVi.rate /1000000,ts1,ts2);
        off64_t pos = findAccuratePageOffset((uint64_t)(timeUs * mVi.rate /1000000),mFirstDataOffset,mFileSize);
#else
    if (mTableOfContents.isEmpty()) {
        // Perform approximate seeking based on avg. bitrate.

        off64_t pos = timeUs * approxBitrate() / 8000000ll;
#endif
        ALOGV("seeking to offset %lld", pos);
        return seekToOffset(pos);
    }

    size_t left = 0;
    size_t right_plus_one = mTableOfContents.size();
    while (left < right_plus_one) {
        size_t center = left + (right_plus_one - left) / 2;

        const TOCEntry &entry = mTableOfContents.itemAt(center);

        if (timeUs < entry.mTimeUs) {
            right_plus_one = center;
        } else if (timeUs > entry.mTimeUs) {
            left = center + 1;
        } else {
            left = center;
            break;
        }
    }

    if (left == mTableOfContents.size()) {
        --left;
    }
    
#ifndef MTK_AOSP_ENHANCEMENT

    const TOCEntry &entry = mTableOfContents.itemAt(left);

    ALOGV("seeking to entry %zu / %zu at offset %lld",
         left, mTableOfContents.size(), entry.mPageOffset);

    return seekToOffset(entry.mPageOffset);
#else
   if(mTableOfContents.itemAt(mTableOfContents.size()-1).mTimeUs <= timeUs)
   	 return seekToOffset(mTableOfContents.itemAt(mTableOfContents.size()-1).mPageOffset);
    off64_t os = 0,oe = 0;
    for(size_t i = left ; i < mTableOfContents.size() ;i ++)
    {
    	  if(mTableOfContents.itemAt(i).mTimeUs > timeUs)
      	  {
      	  	oe = mTableOfContents.itemAt(i).mPageOffset;
      	  	break;
      	  }
      	else if(mTableOfContents.itemAt(i).mTimeUs == timeUs)
      		return seekToOffset(mTableOfContents.itemAt(left).mPageOffset);
    }
    for(size_t i = left ; (i >= 0) && (i < mTableOfContents.size()) ;i --)
    {
    	  if(mTableOfContents.itemAt(i).mTimeUs < timeUs)
      	  {
      	  	os = mTableOfContents.itemAt(i).mPageOffset;
      	  	break;
      	  }
      	else if(mTableOfContents.itemAt(i).mTimeUs == timeUs)
      		return seekToOffset(mTableOfContents.itemAt(left).mPageOffset);
    }
    /*for(size_t i = 0 ; i< mTableOfContents.size() ;i ++)
    {
    	uint64_t ts;
      findGranulePositionofPage(mTableOfContents.itemAt(i).mPageOffset,&ts);
    	SXLOGD("mTableOfContents.itemAt(%d)--mPageOffset:%lld,mTimeUs:%lld,ts:%lld",i,mTableOfContents.itemAt(i).mPageOffset,mTableOfContents.itemAt(i).mTimeUs,ts);
    }*/
    uint64_t ts1,ts2,ts;
    findGranulePositionofPage(os,&ts1);
    findGranulePositionofPage(oe,&ts2);
    off64_t pos = findAccuratePageOffset((uint64_t)(timeUs * mVi.rate /1000000),os,oe);
    findGranulePositionofPage(pos,&ts);
    SXLOGD("seektable seek--pos:%lld,ts1:%lld,ts2:%lld,pos:%lld,ts:%lld",timeUs * mVi.rate /1000000,ts1,ts2,pos,ts);
    return seekToOffset(pos);	
#endif    
}

#ifdef MTK_AOSP_ENHANCEMENT
status_t MyVorbisExtractor::findNextPage_l(
        off64_t startOffset, off64_t *pageOffset) {
    *pageOffset = startOffset;

    char signature[2048];

    for (;;) {
      ssize_t n = mSource->readAt(*pageOffset, &signature, 2000);

        if (n < 4) {
            *pageOffset = 0;

            return (n < 0) ? n : (status_t)ERROR_END_OF_STREAM;
        }
        int i = 0;
        int step = n-4;
        while(i<=step)
        {
            if (!memcmp(&signature[i], "OggS", 4) && ((*pageOffset + i) > startOffset)) {
                if ((*pageOffset + i) > startOffset) {
                    SXLOGV("skipped %lld bytes of junk to reach next frame",
                         (*pageOffset + i) - startOffset);
                }
                *pageOffset += i;
                return OK;
            }
            
            i++;
         }
        *pageOffset += n;    
    }
}
status_t MyVorbisExtractor::findGranulePositionofPage(off64_t offset,uint64_t *granulePositionofPage)
{
	 if (mFirstDataOffset >= 0 && offset < mFirstDataOffset) {
        // Once we know where the actual audio data starts (past the headers)
        // don't ever seek to anywhere before that.
        offset = mFirstDataOffset;
    }
   if(((uint64_t)offset) == mFileSize)
   	{
   		findPrevGranulePosition(mFileSize, granulePositionofPage);
   		if((*granulePositionofPage) < 0xFFFFFFFFFFFF)
   		  return OK;
   		else 
   		  return UNKNOWN_ERROR;    		
   	}
   	
    off64_t pageOffset;
    status_t err = findNextPage_l(offset, &pageOffset);

    if (err != OK) {
    	  err = findNextPage(offset, &pageOffset);
    	  if((err == OK) && (offset == pageOffset))
    	  {
    	  	Page page;
          readPage(offset, &page);
          *granulePositionofPage = page.mGranulePosition;
    	  }
    	  else
    	    findPrevGranulePosition(mFileSize, granulePositionofPage);
   		  if((*granulePositionofPage) < 0xFFFFFFFFFFFF)
   		    return OK;
   		  else 
   			return UNKNOWN_ERROR; 
    }
    // We found the page we wanted to seek to, but we'll also need
    // the page preceding it to determine how many valid samples are on
    // this page.
    findPrevGranulePosition(pageOffset, granulePositionofPage);
 		if((*granulePositionofPage) < 0xFFFFFFFFFFFF)
   		  return OK;
 		else 
   	      return UNKNOWN_ERROR; 
}

off64_t MyVorbisExtractor::findAccuratePageOffset(uint64_t samples,off64_t ofStart,off64_t ofEnd)
{
	  uint64_t sgranulePosition,egranulePosition,granulePositionGuess;
	  off64_t  offsetGuess;
    
  	findGranulePositionofPage(ofStart, &sgranulePosition); 	  
	  findGranulePositionofPage(ofEnd, &egranulePosition);  	 
	  SXLOGD("ofStart:%lld,sgranulePosition:%lld,ofEnd:%lld,egranulePosition:%lld,samples:%lld",ofStart,sgranulePosition,ofEnd,egranulePosition,samples);
	  if((sgranulePosition == egranulePosition)||(samples <= sgranulePosition))
	  	 return ofStart;
	  if(samples >= egranulePosition)
	  	 return ofEnd;
	  offsetGuess = (ofEnd *(samples - sgranulePosition) + ofStart * (egranulePosition - samples))/(egranulePosition-sgranulePosition);
	  if(findGranulePositionofPage(offsetGuess, &granulePositionGuess) != OK)
	  {
	    SXLOGD("offsetGuess is abnormal,return the start offset");
	    return ofStart;
	  }
	  else
    SXLOGD("offsetGuess:%lld,granulePositionGuess:%lld",offsetGuess,granulePositionGuess); 
	  
	  if(samples >= granulePositionGuess)
	  {
	  	 off64_t pageOffset;
       status_t err = findNextPage_l(offsetGuess, &pageOffset);
       if (err != OK) {
       	  return offsetGuess;
       }
       else
       {
         Page page;
         ssize_t n = readPage(pageOffset, &page);
         if (n <= 0) {
            return offsetGuess;
         }
         offsetGuess = pageOffset;
       }
       if(findGranulePositionofPage(offsetGuess, &granulePositionGuess) != OK)
       {
	       SXLOGD("offsetGuess is abnormal,return the start offset");
	       return ofStart;
	     }
       if(samples <= granulePositionGuess)
	  	    return offsetGuess;
	  	 else 
	  	 	return findAccuratePageOffset(samples,offsetGuess,ofEnd); 		
	   }
    else
    {
    	 off64_t pageOffset;
       status_t err = findNextPage_l(ofStart, &pageOffset);
       if (err != OK) {
       	  return offsetGuess;
       }
       else
       {
         Page page;
         ssize_t n = readPage(pageOffset, &page);
         if (n <= 0) {
            return offsetGuess;
         }
         ofStart = pageOffset;
       }
       findGranulePositionofPage(ofStart, &sgranulePosition);
       if(samples <= sgranulePosition)
	  	    return ofStart;
	  	 else 
	  	 	return findAccuratePageOffset(samples,ofStart,offsetGuess);
     }
}
#endif

status_t MyVorbisExtractor::seekToOffset(off64_t offset) {
    if (mFirstDataOffset >= 0 && offset < mFirstDataOffset) {
        // Once we know where the actual audio data starts (past the headers)
        // don't ever seek to anywhere before that.
        offset = mFirstDataOffset;
    }

    off64_t pageOffset;
    status_t err = findNextPage(offset, &pageOffset);

    if (err != OK) {
        return err;
    }

    // We found the page we wanted to seek to, but we'll also need
    // the page preceding it to determine how many valid samples are on
    // this page.
    findPrevGranulePosition(pageOffset, &mPrevGranulePosition);

    mOffset = pageOffset;

    mCurrentPageSize = 0;
    mFirstPacketInPage = true;
    mCurrentPageSamples = 0;
    mCurrentPage.mNumSegments = 0;
    mCurrentPage.mPrevPacketSize = -1;
    mNextLaceIndex = 0;

    // XXX what if new page continues packet from last???

    return OK;
}

ssize_t MyVorbisExtractor::readPage(off64_t offset, Page *page) {
    uint8_t header[27];
    ssize_t n;
    if ((n = mSource->readAt(offset, header, sizeof(header)))
            < (ssize_t)sizeof(header)) {
        ALOGV("failed to read %zu bytes at offset 0x%016llx, got %zd bytes",
             sizeof(header), offset, n);

        if (n < 0) {
            return n;
        } else if (n == 0) {
            return ERROR_END_OF_STREAM;
        } else {
            return ERROR_IO;
        }
    }

    if (memcmp(header, "OggS", 4)) {
        return ERROR_MALFORMED;
    }

    if (header[4] != 0) {
        // Wrong version.

        return ERROR_UNSUPPORTED;
    }

    page->mFlags = header[5];

    if (page->mFlags & ~7) {
        // Only bits 0-2 are defined in version 0.
        return ERROR_MALFORMED;
    }

    page->mGranulePosition = U64LE_AT(&header[6]);

#if 0
    printf("granulePosition = %llu (0x%llx)\n",
           page->mGranulePosition, page->mGranulePosition);
#endif

    page->mSerialNo = U32LE_AT(&header[14]);
    page->mPageNo = U32LE_AT(&header[18]);

    page->mNumSegments = header[26];
    if (mSource->readAt(
                offset + sizeof(header), page->mLace, page->mNumSegments)
            < (ssize_t)page->mNumSegments) {
        return ERROR_IO;
    }

    size_t totalSize = 0;;
    for (size_t i = 0; i < page->mNumSegments; ++i) {
        totalSize += page->mLace[i];
    }

#if 0
    String8 tmp;
    for (size_t i = 0; i < page->mNumSegments; ++i) {
        char x[32];
        sprintf(x, "%s%u", i > 0 ? ", " : "", (unsigned)page->mLace[i]);

        tmp.append(x);
    }

    ALOGV("%c %s", page->mFlags & 1 ? '+' : ' ', tmp.string());
#endif

    return sizeof(header) + page->mNumSegments + totalSize;
}

status_t MyVorbisExtractor::readNextPacket(MediaBuffer **out, bool conf) {
    *out = NULL;

    MediaBuffer *buffer = NULL;
    int64_t timeUs = -1;

    for (;;) {
        size_t i;
        size_t packetSize = 0;
        bool gotFullPacket = false;
        for (i = mNextLaceIndex; i < mCurrentPage.mNumSegments; ++i) {
            uint8_t lace = mCurrentPage.mLace[i];

            packetSize += lace;

            if (lace < 255) {
                gotFullPacket = true;
                ++i;
                break;
            }
        }

        if (mNextLaceIndex < mCurrentPage.mNumSegments) {
            off64_t dataOffset = mOffset + 27 + mCurrentPage.mNumSegments;
            for (size_t j = 0; j < mNextLaceIndex; ++j) {
                dataOffset += mCurrentPage.mLace[j];
            }

            size_t fullSize = packetSize;
            if (buffer != NULL) {
                fullSize += buffer->range_length();
            }
            MediaBuffer *tmp = new MediaBuffer(fullSize);
            if (buffer != NULL) {
                memcpy(tmp->data(), buffer->data(), buffer->range_length());
                tmp->set_range(0, buffer->range_length());
                buffer->release();
            } else {
                // XXX Not only is this not technically the correct time for
                // this packet, we also stamp every packet in this page
                // with the same time. This needs fixing later.

                if (mVi.rate) {
                    // Rate may not have been initialized yet if we're currently
                    // reading the configuration packets...
                    // Fortunately, the timestamp doesn't matter for those.
#ifdef MTK_AOSP_ENHANCEMENT
                    timeUs = (mCurrentPage.mGranulePosition - mCurrentPageSamples) * 1000000ll / mVi.rate;//mPrevGranulePosition
#else                   
                    timeUs = mCurrentPage.mGranulePosition * 1000000ll / mVi.rate;
#endif
                }
                tmp->set_range(0, 0);
            }
            buffer = tmp;

            ssize_t n = mSource->readAt(
                    dataOffset,
                    (uint8_t *)buffer->data() + buffer->range_length(),
                    packetSize);

            if (n < (ssize_t)packetSize) {
                ALOGV("failed to read %zu bytes at 0x%016llx, got %zd bytes",
                     packetSize, dataOffset, n);
                return ERROR_IO;
            }

            buffer->set_range(0, fullSize);

            mNextLaceIndex = i;

            if (gotFullPacket) {
                // We've just read the entire packet.

                if (mFirstPacketInPage) {
                    buffer->meta_data()->setInt32(
                            kKeyValidSamples, mCurrentPageSamples);
                    mFirstPacketInPage = false;
                }

                // ignore timestamp for configuration packets
                if (!conf) {
                    int32_t curBlockSize = packetBlockSize(buffer);
                    if (mCurrentPage.mPrevPacketSize < 0) {
                        mCurrentPage.mPrevPacketSize = curBlockSize;
                        mCurrentPage.mPrevPacketPos =
                                mCurrentPage.mGranulePosition - mCurrentPageSamples;
                        timeUs = mCurrentPage.mPrevPacketPos * 1000000ll / mVi.rate;
                    } else {
                        // The effective block size is the average of the two overlapped blocks
                        int32_t actualBlockSize =
                                (curBlockSize + mCurrentPage.mPrevPacketSize) / 2;
                        timeUs = mCurrentPage.mPrevPacketPos * 1000000ll / mVi.rate;
                        // The actual size output by the decoder will be half the effective
                        // size, due to the overlap
                        mCurrentPage.mPrevPacketPos += actualBlockSize / 2;
                        mCurrentPage.mPrevPacketSize = curBlockSize;
                    }
                    buffer->meta_data()->setInt64(kKeyTime, timeUs);
                }
                *out = buffer;

                return OK;
            }

            // fall through, the buffer now contains the start of the packet.
        }

        CHECK_EQ(mNextLaceIndex, mCurrentPage.mNumSegments);

        mOffset += mCurrentPageSize;
        ssize_t n = readPage(mOffset, &mCurrentPage);

        if (n <= 0) {
            if (buffer) {
                buffer->release();
                buffer = NULL;
            }

            ALOGV("readPage returned %zd", n);

            return n < 0 ? n : (status_t)ERROR_END_OF_STREAM;
        }

        mCurrentPageSamples =
            mCurrentPage.mGranulePosition - mPrevGranulePosition;
        mFirstPacketInPage = true;

#ifdef MTK_AOSP_ENHANCEMENT
        if(mPrevGranulePosition > 0xFFFFFFFFFFFF)
        {
            mCurrentPageSamples = 0;
            SXLOGD("revise the timestamp to page granule position");
        }
#endif

        mPrevGranulePosition = mCurrentPage.mGranulePosition;

        mCurrentPageSize = n;
        mNextLaceIndex = 0;

        if (buffer != NULL) {
            if ((mCurrentPage.mFlags & 1) == 0) {
                // This page does not continue the packet, i.e. the packet
                // is already complete.

                if (timeUs >= 0) {
                    buffer->meta_data()->setInt64(kKeyTime, timeUs);
                }

                buffer->meta_data()->setInt32(
                        kKeyValidSamples, mCurrentPageSamples);
                mFirstPacketInPage = false;

                *out = buffer;

                return OK;
            }
        }
    }
}

status_t MyVorbisExtractor::init() {
    mMeta = new MetaData;
#ifdef MTK_AOSP_ENHANCEMENT
    if(mMeta.get() == NULL) return NO_MEMORY;
#endif
    mMeta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_AUDIO_VORBIS);

    MediaBuffer *packet;
    status_t err;
    if ((err = readNextPacket(&packet, /* conf = */ true)) != OK) {
        return err;
    }
    ALOGV("read packet of size %zu\n", packet->range_length());
    err = verifyHeader(packet, 1);
    packet->release();
    packet = NULL;
    if (err != OK) {
        return err;
    }

    if ((err = readNextPacket(&packet, /* conf = */ true)) != OK) {
        return err;
    }
    ALOGV("read packet of size %zu\n", packet->range_length());
    err = verifyHeader(packet, 3);
    packet->release();
    packet = NULL;
    if (err != OK) {
        return err;
    }

    if ((err = readNextPacket(&packet, /* conf = */ true)) != OK) {
        return err;
    }
    ALOGV("read packet of size %zu\n", packet->range_length());
    err = verifyHeader(packet, 5);
    packet->release();
    packet = NULL;
    if (err != OK) {
        return err;
    }

    mFirstDataOffset = mOffset + mCurrentPageSize;

    off64_t size;
    uint64_t lastGranulePosition;
    if (!(mSource->flags() & DataSource::kIsCachingDataSource)
            && mSource->getSize(&size) == OK
            && findPrevGranulePosition(size, &lastGranulePosition) == OK) {
        // Let's assume it's cheap to seek to the end.
        // The granule position of the final page in the stream will
        // give us the exact duration of the content, something that
        // we can only approximate using avg. bitrate if seeking to
        // the end is too expensive or impossible (live streaming).

        int64_t durationUs = lastGranulePosition * 1000000ll / mVi.rate;

        mMeta->setInt64(kKeyDuration, durationUs);

#ifndef MTK_AOSP_ENHANCEMENT
        buildTableOfContents();//move toc build to start()
#else
        mFileSize = size;
#endif

    }

    return OK;
}

void MyVorbisExtractor::buildTableOfContents() {
    off64_t offset = mFirstDataOffset;
    Page page;
    ssize_t pageSize;
#ifndef MTK_AOSP_ENHANCEMENT
    while ((pageSize = readPage(offset, &page)) > 0) {
#else
	struct timeval tb,te;
	gettimeofday(&tb,NULL);
    while (mTocStarted && ((pageSize = readPage(offset, &page)) > 0)) {
        if(page.mGranulePosition < 0xFFFFFFFFFFFF)
        {
#endif
        mTableOfContents.push();

        TOCEntry &entry =
            mTableOfContents.editItemAt(mTableOfContents.size() - 1);

        entry.mPageOffset = offset;
        entry.mTimeUs = page.mGranulePosition * 1000000ll / mVi.rate;
#ifdef MTK_AOSP_ENHANCEMENT
        //sleep 100ms for consumes over 2s
        gettimeofday(&te,NULL);
        if((te.tv_sec - tb.tv_sec) > 2)
         {
        	  gettimeofday(&tb,NULL);
	          usleep(100000);	  
         }
        }
#endif
        offset += (size_t)pageSize;
    }

    // Limit the maximum amount of RAM we spend on the table of contents,
    // if necessary thin out the table evenly to trim it down to maximum
    // size.

    static const size_t kMaxTOCSize = 8192;
    static const size_t kMaxNumTOCEntries = kMaxTOCSize / sizeof(TOCEntry);

    size_t numerator = mTableOfContents.size();

    if (numerator > kMaxNumTOCEntries) {
        size_t denom = numerator - kMaxNumTOCEntries;

        size_t accum = 0;
        for (ssize_t i = mTableOfContents.size() - 1; i >= 0; --i) {
            accum += denom;
            if (accum >= numerator) {
                mTableOfContents.removeAt(i);
                accum -= numerator;
            }
        }
    }
#ifdef MTK_AOSP_ENHANCEMENT
    mTocDone = true;
#endif
}

int32_t MyVorbisExtractor::packetBlockSize(MediaBuffer *buffer) {
    const uint8_t *data =
        (const uint8_t *)buffer->data() + buffer->range_offset();

    size_t size = buffer->range_length();

    ogg_buffer buf;
    buf.data = (uint8_t *)data;
    buf.size = size;
    buf.refcount = 1;
    buf.ptr.owner = NULL;

    ogg_reference ref;
    ref.buffer = &buf;
    ref.begin = 0;
    ref.length = size;
    ref.next = NULL;

    ogg_packet pack;
    pack.packet = &ref;
    pack.bytes = ref.length;
    pack.b_o_s = 0;
    pack.e_o_s = 0;
    pack.granulepos = 0;
    pack.packetno = 0;

    return vorbis_packet_blocksize(&mVi, &pack);
}

status_t MyVorbisExtractor::verifyHeader(
        MediaBuffer *buffer, uint8_t type) {
    const uint8_t *data =
        (const uint8_t *)buffer->data() + buffer->range_offset();

    size_t size = buffer->range_length();

    if (size < 7 || data[0] != type || memcmp(&data[1], "vorbis", 6)) {
        return ERROR_MALFORMED;
    }

    ogg_buffer buf;
    buf.data = (uint8_t *)data;
    buf.size = size;
    buf.refcount = 1;
    buf.ptr.owner = NULL;

    ogg_reference ref;
    ref.buffer = &buf;
    ref.begin = 0;
    ref.length = size;
    ref.next = NULL;

    oggpack_buffer bits;
    oggpack_readinit(&bits, &ref);

    CHECK_EQ(oggpack_read(&bits, 8), type);
    for (size_t i = 0; i < 6; ++i) {
        oggpack_read(&bits, 8);  // skip 'vorbis'
    }

    switch (type) {
        case 1:
        {
#ifndef MTK_AOSP_ENHANCEMENT    	
            CHECK_EQ(0, _vorbis_unpack_info(&mVi, &bits));
#else
            _vorbis_unpack_info(&mVi, &bits);//skip the CHECK
#endif
            mMeta->setData(kKeyVorbisInfo, 0, data, size);
            mMeta->setInt32(kKeySampleRate, mVi.rate);
            mMeta->setInt32(kKeyChannelCount, mVi.channels);
            
#ifdef MTK_AOSP_ENHANCEMENT
            if(mVi.channels > 2)
            {
#ifndef MTK_SWIP_VORBIS
                SXLOGE("Tremolo does not support multi channel");
                return ERROR_UNSUPPORTED;
#endif
            }
#endif 

            ALOGV("lower-bitrate = %ld", mVi.bitrate_lower);
            ALOGV("upper-bitrate = %ld", mVi.bitrate_upper);
            ALOGV("nominal-bitrate = %ld", mVi.bitrate_nominal);
            ALOGV("window-bitrate = %ld", mVi.bitrate_window);
            ALOGV("blocksizes: %d/%d",
                    vorbis_info_blocksize(&mVi, 0),
                    vorbis_info_blocksize(&mVi, 1)
                    );

            off64_t size;
            if (mSource->getSize(&size) == OK) {
                uint64_t bps = approxBitrate();
                if (bps != 0) {
                    mMeta->setInt64(kKeyDuration, size * 8000000ll / bps);
                }
            }
            break;
        }

        case 3:
        {
            if (0 != _vorbis_unpack_comment(&mVc, &bits)) {
                return ERROR_MALFORMED;
            }

            parseFileMetaData();
            break;
        }

        case 5:
        {
            if (0 != _vorbis_unpack_books(&mVi, &bits)) {
                return ERROR_MALFORMED;
            }

            mMeta->setData(kKeyVorbisBooks, 0, data, size);
            break;
        }
    }

    return OK;
}

uint64_t MyVorbisExtractor::approxBitrate() {
    if (mVi.bitrate_nominal != 0) {
        return mVi.bitrate_nominal;
    }

    return (mVi.bitrate_lower + mVi.bitrate_upper) / 2;
}

void MyVorbisExtractor::parseFileMetaData() {
    mFileMeta = new MetaData;
#ifdef MTK_AOSP_ENHANCEMENT
    if(mFileMeta.get() == NULL)  return;
#endif
    mFileMeta->setCString(kKeyMIMEType, MEDIA_MIMETYPE_CONTAINER_OGG);

    for (int i = 0; i < mVc.comments; ++i) {
        const char *comment = mVc.user_comments[i];
        size_t commentLength = mVc.comment_lengths[i];
        parseVorbisComment(mFileMeta, comment, commentLength);
        //ALOGI("comment #%d: '%s'", i + 1, mVc.user_comments[i]);
    }
}

void parseVorbisComment(
        const sp<MetaData> &fileMeta, const char *comment, size_t commentLength)
{
    struct {
        const char *const mTag;
        uint32_t mKey;
    } kMap[] = {
        { "TITLE", kKeyTitle },
        { "ARTIST", kKeyArtist },
        { "ALBUMARTIST", kKeyAlbumArtist },
        { "ALBUM ARTIST", kKeyAlbumArtist },
        { "COMPILATION", kKeyCompilation },
        { "ALBUM", kKeyAlbum },
        { "COMPOSER", kKeyComposer },
        { "GENRE", kKeyGenre },
        { "AUTHOR", kKeyAuthor },
        { "TRACKNUMBER", kKeyCDTrackNumber },
        { "DISCNUMBER", kKeyDiscNumber },
        { "DATE", kKeyDate },
        { "LYRICIST", kKeyWriter },
        { "METADATA_BLOCK_PICTURE", kKeyAlbumArt },
        { "ANDROID_LOOP", kKeyAutoLoop },
    };

        for (size_t j = 0; j < sizeof(kMap) / sizeof(kMap[0]); ++j) {
            size_t tagLen = strlen(kMap[j].mTag);
            if (!strncasecmp(kMap[j].mTag, comment, tagLen)
                    && comment[tagLen] == '=') {
                if (kMap[j].mKey == kKeyAlbumArt) {
                    extractAlbumArt(
                            fileMeta,
                            &comment[tagLen + 1],
                            commentLength - tagLen - 1);
                } else if (kMap[j].mKey == kKeyAutoLoop) {
                    if (!strcasecmp(&comment[tagLen + 1], "true")) {
                        fileMeta->setInt32(kKeyAutoLoop, true);
                    }
                } else {
                    fileMeta->setCString(kMap[j].mKey, &comment[tagLen + 1]);
                }
            }
        }

}

static void extractAlbumArt(
        const sp<MetaData> &fileMeta, const void *data, size_t size) {
    ALOGV("extractAlbumArt from '%s'", (const char *)data);

    sp<ABuffer> flacBuffer = decodeBase64(AString((const char *)data, size));
    if (flacBuffer == NULL) {
        ALOGE("malformed base64 encoded data.");
        return;
    }

    size_t flacSize = flacBuffer->size();
    uint8_t *flac = flacBuffer->data();
    ALOGV("got flac of size %zu", flacSize);

    uint32_t picType;
    uint32_t typeLen;
    uint32_t descLen;
    uint32_t dataLen;
    char type[128];

    if (flacSize < 8) {
        return;
    }

    picType = U32_AT(flac);

    if (picType != 3) {
        // This is not a front cover.
        return;
    }

    typeLen = U32_AT(&flac[4]);
    if (typeLen > sizeof(type) - 1) {
        return;
    }

    // we've already checked above that flacSize >= 8
    if (flacSize - 8 < typeLen) {
        return;
    }

    memcpy(type, &flac[8], typeLen);
    type[typeLen] = '\0';

    ALOGV("picType = %d, type = '%s'", picType, type);

    if (!strcmp(type, "-->")) {
        // This is not inline cover art, but an external url instead.
        return;
    }

    descLen = U32_AT(&flac[8 + typeLen]);

    if (flacSize < 32 ||
        flacSize - 32 < typeLen ||
        flacSize - 32 - typeLen < descLen) {
        return;
    }

    dataLen = U32_AT(&flac[8 + typeLen + 4 + descLen + 16]);


    // we've already checked above that (flacSize - 32 - typeLen - descLen) >= 0
    if (flacSize - 32 - typeLen - descLen < dataLen) {
        return;
    }

    ALOGV("got image data, %zu trailing bytes",
         flacSize - 32 - typeLen - descLen - dataLen);

    fileMeta->setData(
            kKeyAlbumArt, 0, &flac[8 + typeLen + 4 + descLen + 20], dataLen);

    fileMeta->setCString(kKeyAlbumArtMIME, type);
}

////////////////////////////////////////////////////////////////////////////////

OggExtractor::OggExtractor(const sp<DataSource> &source)
    : mDataSource(source),
      mInitCheck(NO_INIT),
      mImpl(NULL) {
    mImpl = new MyVorbisExtractor(mDataSource);
#ifdef MTK_AOSP_ENHANCEMENT
    if (mImpl == NULL)  return;
#endif
    mInitCheck = mImpl->seekToOffset(0);

    if (mInitCheck == OK) {
        mInitCheck = mImpl->init();
    }
#ifdef MTK_AOSP_ENHANCEMENT
    if (mInitCheck != OK)
       SXLOGE("MyVorbisExtractor init error");
#endif
}

OggExtractor::~OggExtractor() {
#ifdef MTK_AOSP_ENHANCEMENT
	  if (mImpl != NULL)
	  {
#endif
    delete mImpl;
    mImpl = NULL;
#ifdef MTK_AOSP_ENHANCEMENT
	  	}
#endif
}

size_t OggExtractor::countTracks() {
    return mInitCheck != OK ? 0 : 1;
}

sp<MediaSource> OggExtractor::getTrack(size_t index) {
    if (index >= 1) {
        return NULL;
    }

    return new OggSource(this);
}

sp<MetaData> OggExtractor::getTrackMetaData(
        size_t index, uint32_t /* flags */) {
    if (index >= 1) {
        return NULL;
    }

    return mImpl->getFormat();
}

sp<MetaData> OggExtractor::getMetaData() {
    return mImpl->getFileMetaData();
}

bool SniffOgg(
        const sp<DataSource> &source, String8 *mimeType, float *confidence,
        sp<AMessage> *) {
    char tmp[4];
    if (source->readAt(0, tmp, 4) < 4 || memcmp(tmp, "OggS", 4)) {
        return false;
    }

    mimeType->setTo(MEDIA_MIMETYPE_CONTAINER_OGG);
    *confidence = 0.2f;

    return true;
}

}  // namespace android
