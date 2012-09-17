/********************************************************************
 *                                                                  *
 * THIS FILE IS PART OF THE libopusfile SOFTWARE CODEC SOURCE CODE. *
 * USE, DISTRIBUTION AND REPRODUCTION OF THIS LIBRARY SOURCE IS     *
 * GOVERNED BY A BSD-STYLE SOURCE LICENSE INCLUDED WITH THIS SOURCE *
 * IN 'COPYING'. PLEASE READ THESE TERMS BEFORE DISTRIBUTING.       *
 *                                                                  *
 * THE libopusfile SOURCE CODE IS (C) COPYRIGHT 1994-2009           *
 * by the Xiph.Org Foundation and contributors http://www.xiph.org/ *
 *                                                                  *
 ********************************************************************

 function: stdio-based convenience library for opening/seeking/decoding
 last mod: $Id: vorbisfile.c 17573 2010-10-27 14:53:59Z xiphmont $

 ********************************************************************/
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <math.h>

#include "opus/opusfile.h"

/*This implementation is largely based off of libvorbisfile.
  All of the Ogg bits work roughly the same, though I have made some
   "improvements" that have not been folded back there, yet.*/

/*A 'chained bitstream' is an Ogg Opus bitstream that contains more than one
   logical bitstream arranged end to end (the only form of Ogg multiplexing
   supported by this library.
  Grouping (parallel multiplexing) is not supported, except to the extent that
   if there are multiple logical Ogg streams in a single link of the chain, we
   will ignore all but the first Opus stream we find.*/

/*An Ogg Opus file can be played beginning to end (streamed) without worrying
   ahead of time about chaining (see opusdec from the opus-tools package).
  If we have the whole file, however, and want random access
   (seeking/scrubbing) or desire to know the total length/time of a file, we
   need to account for the possibility of chaining.*/

/*We can handle things a number of ways.
  We can determine the entire bitstream structure right off the bat, or find
   pieces on demand.
  This library determines and caches structure for the entire bitstream, but
   builds a virtual decoder on the fly when moving between links in the chain.*/

/*There are also different ways to implement seeking.
  Enough information exists in an Ogg bitstream to seek to sample-granularity
   positions in the output.
  Or, one can seek by picking some portion of the stream roughly in the desired
   area if we only want coarse navigation through the stream.
  We implement and expose both strategies.*/

/*Many, many internal helpers.
  The intention is not to be confusing.
  Rampant duplication and monolithic function implementation (though we do have
   some large, omnibus functions still) would be harder to understand anyway.
  The high level functions are last.
  Begin grokking near the end of the file if you prefer to read things
   top-down.*/

/*The maximum number of bytes in a page (including the page headers).*/
#define OP_PAGE_SIZE      (65307)
/*The default amount to seek backwards per step when trying to find the
   previous page.
  This must be at least as large as the maximum size of a page.*/
#define OP_CHUNK_SIZE     (65536)
/*The maximum amount to seek backwards per step when trying to find the
   previous page.*/
#define OP_CHUNK_SIZE_MAX (1024*1024)
/*A smaller read size is needed for low-rate streaming.*/
#define OP_READ_SIZE      (2048)

int op_test(OpusHead *_head,
 const unsigned char *_initial_data,size_t _initial_bytes){
  ogg_sync_state  oy;
  char           *data;
  int             err;
  /*The first page of a normal Opus file will be at most 57 bytes (27 Ogg
     page header bytes + 1 lacing value + 21 Opus header bytes + 8 channel
     mapping bytes).
    It will be at least 47 bytes (27 Ogg page header bytes + 1 lacing value +
     19 Opus header bytes using channel mapping family 0).
    If we don't have at least that much data, give up now.*/
  if(_initial_bytes<47)return OP_FALSE;
  /*Only proceed if we start with the magic OggS string.
    This is to prevent us spending a lot of time allocating memory and looking
     for Ogg pages in non-Ogg files.*/
  if(memcmp(_initial_data,"OggS",4)!=0)return OP_ENOTFORMAT;
  ogg_sync_init(&oy);
  data=ogg_sync_buffer(&oy,_initial_bytes);
  if(data!=NULL){
    ogg_stream_state os;
    ogg_page         og;
    int              ret;
    memcpy(data,_initial_data,_initial_bytes);
    ogg_sync_wrote(&oy,_initial_bytes);
    ogg_stream_init(&os,-1);
    err=OP_FALSE;
    do{
      ogg_packet op;
      ret=ogg_sync_pageout(&oy,&og);
      /*Ignore holes.*/
      if(ret<0)continue;
      /*Stop if we run out of data.*/
      if(!ret)break;
      ogg_stream_reset_serialno(&os,ogg_page_serialno(&og));
      ogg_stream_pagein(&os,&og);
      /*Only process the first packet on this page (if it's a BOS packet,
         it's required to be the only one).*/
      if(ogg_stream_packetout(&os,&op)==1){
        if(op.b_o_s){
          ret=opus_head_parse(_head,op.packet,op.bytes);
          /*If this didn't look like Opus, keep going.*/
          if(ret==OP_ENOTFORMAT)continue;
          /*Otherwise we're done, one way or another.*/
          err=ret;
        }
        /*We finished parsing the headers.
          There is no Opus to be found.*/
        else err=OP_ENOTFORMAT;
      }
    }
    while(err==OP_FALSE);
    ogg_stream_clear(&os);
  }
  else err=OP_EFAULT;
  ogg_sync_clear(&oy);
  return err;
}

/*Read a little more data from the file/pipe into the ogg_sync framer.*/
static int op_get_data(OggOpusFile *_of){
  char *buffer;
  int   bytes;
  buffer=ogg_sync_buffer(&_of->oy,OP_READ_SIZE);
  bytes=(int)(*_of->callbacks.read)(buffer,
   1,OP_READ_SIZE,_of->source);
  if(OP_LIKELY(bytes>0)){
    ogg_sync_wrote(&_of->oy,bytes);
    return bytes;
  }
  return OP_EREAD;
}

/*Save a tiny smidge of verbosity to make the code more readable.*/
static int op_seek_helper(OggOpusFile *_of,opus_int64 _offset){
  if(_of->callbacks.seek==NULL||
   (*_of->callbacks.seek)(_of->source,_offset,SEEK_SET)){
    return OP_EREAD;
  }
  _of->offset=_offset;
  ogg_sync_reset(&_of->oy);
  return 0;
}

/*The read/seek functions track absolute position within the stream.*/

/*From the head of the stream, get the next page.
  _boundary specifies if the function is allowed to fetch more data from the
   stream (and how much) or only use internally buffered data.
  _boundary: -1) Unbounded search.
              0) Read no additional data.
                 Use only cached data.
              n) Search for the start of a new page for n bytes.
  Return: n>=0)     Found a page at absolute offset n.
          OP_FALSE) Hit the _boundary limit.
          OP_EREAD) Failed to read more data.*/
static opus_int64 op_get_next_page(OggOpusFile *_of,ogg_page *_og,
 opus_int64 _boundary){
  if(_boundary>0)_boundary+=_of->offset;
  for(;;){
    int more;
    if(_boundary>0&&_of->offset>=_boundary)return OP_FALSE;
    more=ogg_sync_pageseek(&_of->oy,_og);
    /*Skipped (-more) bytes.*/
    if(OP_UNLIKELY(more<0))_of->offset-=more;
    else if(more==0){
      int ret;
      /*Send more paramedics.*/
      if(!_boundary)return OP_FALSE;
      ret=op_get_data(_of);
      if(OP_UNLIKELY(ret<0))return ret;
    }
    else{
      /*Got a page.
        Return the offset at the page beginning, advance the internal offset
         past the page end.*/
      opus_int64 page_offset;
      page_offset=_of->offset;
      _of->offset+=more;
      return page_offset;
    }
  }
}

static int op_add_serialno(ogg_page *_og,
 ogg_uint32_t **_serialnos,int *_nserialnos,int *_cserialnos){
  ogg_uint32_t *serialnos;
  int           nserialnos;
  int           cserialnos;
  ogg_uint32_t s;
  s=ogg_page_serialno(_og);
  serialnos=*_serialnos;
  nserialnos=*_nserialnos;
  cserialnos=*_cserialnos;
  if(OP_UNLIKELY(nserialnos>=cserialnos)){
    if(OP_UNLIKELY(cserialnos>INT_MAX-1>>1))return OP_EFAULT;
    cserialnos=2*cserialnos+1;
    OP_ASSERT(nserialnos<cserialnos);
    serialnos=_ogg_realloc(serialnos,sizeof(*serialnos)*cserialnos);
    if(OP_UNLIKELY(serialnos==NULL))return OP_EFAULT;
  }
  serialnos[nserialnos++]=s;
  *_serialnos=serialnos;
  *_nserialnos=nserialnos;
  *_cserialnos=cserialnos;
  return 0;
}

/*Returns nonzero if found.*/
static int op_lookup_serialno(ogg_uint32_t _s,
 const ogg_uint32_t *_serialnos,int _nserialnos){
  int i;
  for(i=0;i<_nserialnos&&_serialnos[i]!=_s;i++);
  return i<_nserialnos;
}

static int op_lookup_page_serialno(ogg_page *_og,
 const ogg_uint32_t *_serialnos,int _nserialnos){
  return op_lookup_serialno(ogg_page_serialno(_og),_serialnos,_nserialnos);
}

/*Find the last page beginning before the current stream cursor position with a
   valid granule position.
  There is no '_boundary' parameter as it will have to read more data.
  This is much dirtier than the above, as Ogg doesn't have any backward search
   linkage.
  This search prefers pages of the specified serial number.
  If a page of the specified serial number is spotted during the
   seek-back-and-read-forward, it will return the info of last page of the
   matching serial number, instead of the very last page.
  If no page of the specified serial number is seen, it will return the info of
   the last page and update *_serialno.
  Return: The offset of the start of the page, or a negative value on failure.
          OP_EREAD:    Failed to read more data (error or EOF).
          OP_EBADLINK: We couldn't find a page even after seeking back to the
                        start of the stream.*/
static opus_int64 op_get_prev_page_serial(OggOpusFile *_of,
 const ogg_uint32_t *_serialnos,int _nserialnos,opus_int32 *_chunk_size,
 ogg_uint32_t *_serialno,ogg_int64_t *_gp){
  ogg_page     og;
  opus_int64   begin;
  opus_int64   end;
  opus_int64   original_end;
  opus_int64   offset;
  opus_int64   preferred_offset;
  ogg_uint32_t preferred_serialno;
  ogg_int64_t  ret_serialno;
  ogg_int64_t  ret_gp;
  opus_int32   chunk_size;
  original_end=end=begin=_of->offset;
  preferred_offset=offset=-1;
  ret_serialno=-1;
  ret_gp=-1;
  preferred_serialno=*_serialno;
  chunk_size=_chunk_size==NULL?OP_CHUNK_SIZE:*_chunk_size;
  do{
    int ret;
    OP_ASSERT(chunk_size>=OP_PAGE_SIZE);
    begin=OP_MAX(begin-chunk_size,0);
    ret=op_seek_helper(_of,begin);
    if(OP_UNLIKELY(ret<0))return ret;
    while(_of->offset<end){
      opus_int64 llret;
      llret=op_get_next_page(_of,&og,end-_of->offset);
      if(OP_UNLIKELY(llret<OP_FALSE))return llret;
      else if(llret==OP_FALSE)break;
      ret_serialno=ogg_page_serialno(&og);
      ret_gp=ogg_page_granulepos(&og);
      offset=llret;
      if(ret_serialno==preferred_serialno){
        preferred_offset=offset;
        *_gp=ret_gp;
      }
      if(!op_lookup_serialno(ret_serialno,_serialnos,_nserialnos)){
        /*We fell off the end of the link, which means we seeked back too far
           and shouldn't have been looking in that link to begin with.
          If we found the preferred serial number, forget that we saw it.*/
        preferred_offset=-1;
      }
    }
    /*We started from the beginning of the stream and found nothing.
      This should be impossible unless the contents of the source changed out
       from under us after we read from it.*/
    if(OP_UNLIKELY(!begin)&&OP_UNLIKELY(offset==-1))return OP_EBADLINK;
    /*Bump up the chunk size.
      This is mildly helpful when seeks are very expensive (http).*/
    chunk_size=OP_MIN(2*chunk_size,OP_CHUNK_SIZE_MAX);
    /*Avoid quadratic complexity if we hit an invalid patch of the file.*/
    end=OP_MIN(begin+OP_PAGE_SIZE-1,original_end);
  }
  while(offset==-1);
  if(_chunk_size!=NULL)*_chunk_size=chunk_size;
  /*We're not interested in the page... just the serial number, byte offset,
    and granule position.*/
  if(preferred_offset>=0)return preferred_offset;
  *_serialno=ret_serialno;
  *_gp=ret_gp;
  return offset;
}

/*Uses the local ogg_stream storage in _of.
  This is important for non-streaming input sources.*/
static int op_fetch_headers_impl(OggOpusFile *_of,OpusHead *_head,
 OpusTags *_tags,ogg_uint32_t **_serialnos,int *_nserialnos,
 int *_cserialnos,ogg_page *_og){
  ogg_packet op;
  int        ret;
  if(_serialnos!=NULL)*_nserialnos=0;
  /*Extract the serialnos of all BOS pages plus the first set of Opus headers
     we see in the link.*/
  while(ogg_page_bos(_og)){
    opus_int64 llret;
    if(_serialnos!=NULL){
      if(OP_UNLIKELY(op_lookup_page_serialno(_og,*_serialnos,*_nserialnos))){
        /*A dupe serialnumber in an initial header packet set==invalid stream.*/
        return OP_EBADHEADER;
      }
      ret=op_add_serialno(_og,_serialnos,_nserialnos,_cserialnos);
      if(OP_UNLIKELY(ret<0))return ret;
    }
    if(_of->ready_state<OP_STREAMSET){
      /*We don't have an Opus stream in this link yet, so begin prospective
         stream setup.
        We need a stream to get packets.*/
      ogg_stream_reset_serialno(&_of->os,ogg_page_serialno(_og));
      ogg_stream_pagein(&_of->os,_og);
      if(OP_LIKELY(ogg_stream_packetout(&_of->os,&op)>0)){
        ret=opus_head_parse(_head,op.packet,op.bytes);
        /*If it's just a stream type we don't recognize, ignore it.*/
        if(ret==OP_ENOTFORMAT)continue;
        /*Everything else is fatal.*/
        if(OP_UNLIKELY(ret<0))return ret;
        /*Found a valid Opus header.
          Continue setup.*/
        _of->ready_state=OP_STREAMSET;
      }
    }
    /*Get the next page.*/
    llret=op_get_next_page(_of,_og,OP_CHUNK_SIZE);
    if(OP_UNLIKELY(llret<0))return OP_ENOTFORMAT;
    /*If this page also belongs to our Opus stream, submit it and break.*/
    if(_of->ready_state==OP_STREAMSET
     &&_of->os.serialno==ogg_page_serialno(_og)){
      ogg_stream_pagein(&_of->os,_og);
      break;
    }
  }
  if(OP_UNLIKELY(_of->ready_state!=OP_STREAMSET))return OP_ENOTFORMAT;
  /*Loop getting packets.*/
  for(;;){
    switch(ogg_stream_packetout(&_of->os,&op)){
      case 0:{
        /*Loop getting pages.*/
        for(;;){
          if(OP_UNLIKELY(op_get_next_page(_of,_og,OP_CHUNK_SIZE)<0)){
            return OP_EBADHEADER;
          }
          /*If this page belongs to the correct stream, go parse it.*/
          if(_of->os.serialno==ogg_page_serialno(_og)){
            ogg_stream_pagein(&_of->os,_og);
            break;
          }
          /*If the link ends before we see the Opus comment header, abort.*/
          if(OP_UNLIKELY(ogg_page_bos(_og)))return OP_EBADHEADER;
          /*Otherwise, keep looking.*/
        }
      }break;
      /*We shouldn't get a hole in the headers!*/
      case -1:return OP_EBADHEADER;
      default:{
        /*Got a packet.
          It should be the comment header.*/
        ret=opus_tags_parse(_tags,op.packet,op.bytes);
        if(OP_UNLIKELY(ret<0))return ret;
        /*Make sure the page terminated at the end of the comment header.
          If there is another packet on the page, or part of a packet, then
           reject the stream.
          Otherwise seekable sources won't be able to seek back to the start
           properly.*/
        ret=ogg_stream_packetout(&_of->os,&op);
        if(OP_UNLIKELY(ret!=0)
         ||OP_UNLIKELY(_og->header[_og->header_len-1]==255)){
          /*If we fail, the caller assumes our tags are uninitialized.*/
          opus_tags_clear(_tags);
          return OP_EBADHEADER;
        }
        return 0;
      }
    }
  }
}

static int op_fetch_headers(OggOpusFile *_of,OpusHead *_head,
 OpusTags *_tags,ogg_uint32_t **_serialnos,int *_nserialnos,
 int *_cserialnos,ogg_page *_og){
  ogg_page og;
  int      ret;
  if(!_og){
    ogg_int64_t llret;
    llret=op_get_next_page(_of,&og,OP_CHUNK_SIZE);
    if(OP_UNLIKELY(llret<0))return OP_ENOTFORMAT;
    _og=&og;
  }
  _of->ready_state=OP_OPENED;
  ret=op_fetch_headers_impl(_of,_head,_tags,_serialnos,_nserialnos,
   _cserialnos,_og);
  /*Revert back from OP_STREAMSET to OP_OPENED on failure, to prevent
     double-free of the tags in an unseekable stream.*/
  if(OP_UNLIKELY(ret<0))_of->ready_state=OP_OPENED;
  return ret;
}

/*Granule position manipulation routines.
  A granule position is defined to be an unsigned 64-bit integer, with the
   special value -1 in two's complement indicating an unset or invalid granule
   position.
  We are not guaranteed to have an unsigned 64-bit type, so we construct the
   following routines that
   a) Properly order negative numbers as larger than positive numbers, and
   b) Check for underflow or overflow past the special -1 value.
  This lets us operate on the full, valid range of granule positions in a
   consistent and safe manner.
  This full range is organized into distinct regions:
   [ -1 (invalid) ][ 0 ... OP_INT64_MAX ][ OP_INT64_MIN ... -2 ][-1 (invalid) ]

  No one should actually use granule positions so large that they're negative,
   even if they are technically valid, as very little software handles them
   correctly (including most of Xiph.Org's).
  This library also refuses to support durations so large they won't fit in a
   signed 64-bit integer (to avoid exposing this mess to the application, and
   to simplify a good deal of internal arithmetic), so the only way to use them
   successfully is if pcm_start is very large.
  This means there isn't anything you can do with negative granule positions
   that you couldn't have done with purely non-negative ones.
  The main purpose of these routines is to allow us to think very explicitly
   about the possible failure cases of all granule position manipulations.*/

/*Safely adds a small signed integer to a valid (not -1) granule position.
  The result can use the full 64-bit range of values (both positive and
   negative), but will fail on overflow (wrapping past -1; wrapping past
   OP_INT64_MAX is explicitly okay).
  [out] _dst_gp: The resulting granule position.
                 Only modified on success.
  _src_gp:       The granule position to add to.
                 This must not be -1.
  _delta:        The amount to add.
                 This is allowed to be up to 32 bits to support the maximum
                  duration of a single Ogg page (255 packets * 120 ms per
                  packet == 1,468,800 samples at 48 kHz).
  Return: 0 on success, or OP_EINVAL if the result would wrap around past -1.*/
static int op_granpos_add(ogg_int64_t *_dst_gp,ogg_int64_t _src_gp,
 opus_int32 _delta){
  /*The code below handles this case correctly, but there's no reason we
     should ever be called with these values, so make sure we aren't.*/
  OP_ASSERT(_src_gp!=-1);
  if(_delta>0){
    /*Adding this amount to the granule position would overflow its 64-bit
       range.*/
    if(OP_UNLIKELY(_src_gp<0)&&OP_UNLIKELY(_src_gp>=-1-_delta))return OP_EINVAL;
    if(OP_UNLIKELY(_src_gp>OP_INT64_MAX-_delta)){
      /*Adding this amount to the granule position would overflow the positive
         half of its 64-bit range.
        Since signed overflow is undefined in C, do it in a way the compiler
         isn't allowed to screw up.*/
      _delta-=(opus_int32)(OP_INT64_MAX-_src_gp)+1;
      _src_gp=OP_INT64_MIN;
    }
  }
  else if(_delta<0){
    /*Subtracting this amount from the granule position would underflow its
       64-bit range.*/
    if(_src_gp>=0&&OP_UNLIKELY(_src_gp<-_delta))return OP_EINVAL;
    if(OP_UNLIKELY(_src_gp<OP_INT64_MIN-_delta)){
      /*Subtracting this amount from the granule position would underflow the
         negative half of its 64-bit range.
        Since signed underflow is undefined in C, do it in a way the compiler
         isn't allowed to screw up.*/
      _delta+=(opus_int32)(_src_gp-OP_INT64_MIN)+1;
      _src_gp=OP_INT64_MAX;
    }
  }
  *_dst_gp=_src_gp+_delta;
  return 0;
}

/*Safely computes the difference between two granule positions.
  The difference must fit in a signed 64-bit integer, or the function fails.
  It correctly handles the case where the granule position has wrapped around
   from positive values to negative ones.
  [out] _delta: The difference between the granule positions.
                Only modified on success.
  _gp_a:        The granule position to subtract from.
                This must not be -1.
  _gp_b:        The granule position to subtract.
                This must not be -1.
  Return: 0 on success, or OP_EINVAL if the result would not fit in a signed
           64-bit integer.*/
static int op_granpos_diff(ogg_int64_t *_delta,
 ogg_int64_t _gp_a,ogg_int64_t _gp_b){
  int gp_a_negative;
  int gp_b_negative;
  /*The code below handles these cases correctly, but there's no reason we
     should ever be called with these values, so make sure we aren't.*/
  OP_ASSERT(_gp_a!=-1);
  OP_ASSERT(_gp_b!=-1);
  gp_a_negative=OP_UNLIKELY(_gp_a<0);
  gp_b_negative=OP_UNLIKELY(_gp_b<0);
  if(OP_UNLIKELY(gp_a_negative^gp_b_negative)){
    ogg_int64_t da;
    ogg_int64_t db;
    if(gp_a_negative){
      /*_gp_a has wrapped to a negative value but _gp_b hasn't: the difference
         should be positive.*/
      /*Step 1: Handle wrapping.*/
      /*_gp_a < 0 => da < 0.*/
      da=(OP_INT64_MIN-_gp_a)-1;
      /*_gp_b >= 0  => db >= 0.*/
      db=OP_INT64_MAX-_gp_b;
      /*Step 2: Check for overflow.*/
      if(OP_UNLIKELY(OP_INT64_MAX+da<db))return OP_EINVAL;
      *_delta=db-da;
    }
    else{
      /*_gp_b has wrapped to a negative value but _gp_a hasn't: the difference
         should be negative.*/
      /*Step 1: Handle wrapping.*/
      /*_gp_a >= 0 => da <= 0*/
      da=_gp_a+OP_INT64_MIN;
      /*_gp_b < 0 => db <= 0*/
      db=OP_INT64_MIN-_gp_b;
      /*Step 2: Check for overflow.*/
      if(OP_UNLIKELY(da<OP_INT64_MIN-db))return OP_EINVAL;
      *_delta=da+db;
    }
  }
  else *_delta=_gp_a-_gp_b;
  return 0;
}

static int op_granpos_cmp(ogg_int64_t _gp_a,ogg_int64_t _gp_b){
  /*The invalid granule position -1 should behave like NaN: neither greater
     than nor less than any other granule position, nor equal to any other
     granule position, including itself.
    However, that means there isn't anything we could sensibly return from this
     function for it.*/
  OP_ASSERT(_gp_a!=-1);
  OP_ASSERT(_gp_b!=-1);
  /*Handle the wrapping cases.*/
  if(OP_UNLIKELY(_gp_a<0)){
    if(_gp_b>=0)return 1;
    /*Else fall through.*/
  }
  else if(OP_UNLIKELY(_gp_b<0))return -1;
  /*No wrapping case.*/
  return (_gp_a>_gp_b)-(_gp_b>_gp_a);
}

/*Returns the duration of the packet (in samples at 48 kHz), or a negative
   value on error.*/
static int op_get_packet_duration(const unsigned char *_data,int _len){
  int nframes;
  int frame_size;
  int nsamples;
  nframes=opus_packet_get_nb_frames(_data,_len);
  if(OP_UNLIKELY(nframes<0))return OP_EBADPACKET;
  frame_size=opus_packet_get_samples_per_frame(_data,48000);
  nsamples=nframes*frame_size;
  if(OP_UNLIKELY(nsamples>120*48))return OP_EBADPACKET;
  return nsamples;
}

/*This function more properly belongs in info.c, but we define it here to allow
   the static granule position manipulation functions to remain static.*/
ogg_int64_t opus_granule_sample(const OpusHead *_head,ogg_int64_t _gp){
  opus_int32 pre_skip;
  pre_skip=_head->pre_skip;
  if(_gp!=-1&&op_granpos_add(&_gp,_gp,-pre_skip))_gp=-1;
  return _gp;
}

/*Grab all the packets currently in the stream state, and compute their
   durations.
  _of->op_count is set to the number of packets collected.
  [out] _durations: Returns the durations of the individual packets.
  Return: The total duration of all packets, or OP_HOLE if there was a hole.*/
static opus_int32 op_collect_audio_packets(OggOpusFile *_of,
 int _durations[255]){
  opus_int32 total_duration;
  int        op_count;
  /*Count the durations of all packets in the page.*/
  op_count=0;
  total_duration=0;
  for(;;){
    int ret;
    /*Unless libogg is broken, we can't get more than 255 packets from a
       single page.*/
    OP_ASSERT(op_count<255);
    /*This takes advantage of undocumented libogg behavior that returned
       ogg_packet buffers are valid at least until the next page is
       submitted.
      Relying on this is not too terrible, as _none_ of the Ogg memory
       ownership/lifetime rules are well-documented.
      But I can read its code and know this will work.*/
    ret=ogg_stream_packetout(&_of->os,_of->op+op_count);
    if(!ret)break;
    if(OP_UNLIKELY(ret<0)){
      /*We shouldn't get holes in the middle of pages.*/
      OP_ASSERT(op_count==0);
      return OP_HOLE;
    }
    _durations[op_count]=op_get_packet_duration(_of->op[op_count].packet,
     _of->op[op_count].bytes);
    if(OP_LIKELY(_durations[op_count]>0)){
      /*With at most 255 packets on a page, this can't overflow.*/
      total_duration+=_durations[op_count++];
    }
    /*Ignore packets with an invalid TOC sequence.*/
  }
  _of->op_pos=0;
  _of->op_count=op_count;
  return total_duration;
}

/*Starting from current cursor position, get the initial PCM offset of the next
   page.
  This also validates the granule position on the first page with a completed
   audio data packet, as required by the spec.
  If this link is completely empty (no pages with completed packets), then this
   function sets pcm_start=pcm_end=0 and returns the BOS page of the next link
   (if any).
  In the seekable case, we initialize pcm_end=-1 before calling this function,
   so that later we can detect that the link was empty before calling
   op_find_final_pcm_offset().
  [inout] _link: The link for which to find pcm_start.
  [out] _og:     Returns the BOS page of the next link if this link was empty.
                 In the unseekable case, we can then feed this to
                  op_fetch_headers() to start the next link.
                 The caller may pass NULL (e.g., for seekable streams), in
                  which case this page will be discarded.
  Return: 0 on success, 1 if there is a buffered BOS page available, or a
           negative value on unrecoverable error.*/
static int op_find_initial_pcm_offset(OggOpusFile *_of,
 OggOpusLink *_link,ogg_page *_og){
  ogg_page     og;
  ogg_int64_t  pcm_start;
  ogg_int64_t  prev_packet_gp;
  ogg_int64_t  cur_page_gp;
  ogg_uint32_t serialno;
  opus_int32   total_duration;
  int          durations[255];
  int          cur_page_eos;
  int          op_count;
  int          pi;
  if(_og==NULL)_og=&og;
  serialno=_of->os.serialno;
  cur_page_gp=-1;
  do{
    /*We should get a page unless the file is truncated or mangled.
      Otherwise there are no audio data packets in the whole logical stream.*/
    if(OP_UNLIKELY(op_get_next_page(_of,_og,-1)<0)){
      /*Fail if the pre-skip is non-zero, since it's asking us to skip more
         samples than exist.*/
      if(_link->head.pre_skip>0)return OP_EBADTIMESTAMP;
      /*Set pcm_end and end_offset so we can skip the call to
         op_find_final_pcm_offset().*/
      _link->pcm_start=_link->pcm_end=0;
      _link->end_offset=_link->data_offset;
      return 0;
    }
    /*Similarly, if we hit the next link in the chain, we've gone too far.*/
    if(OP_UNLIKELY(ogg_page_bos(_og))){
      if(_link->head.pre_skip>0)return OP_EBADTIMESTAMP;
      /*Set pcm_end and end_offset so we can skip the call to
         op_find_final_pcm_offset().*/
      _link->pcm_end=_link->pcm_start=0;
      _link->end_offset=_link->data_offset;
      /*Tell the caller we've got a buffered page for them.*/
      return 1;
    }
    /*Ignore pages from other streams (not strictly necessary, because of the
       checks in ogg_stream_pagein(), but saves some work).*/
    if(serialno!=(ogg_uint32_t)ogg_page_serialno(_og))continue;
    ogg_stream_pagein(&_of->os,_og);
    /*Bitrate tracking: add the header's bytes here.
      The body bytes are counted when we consume the packets.*/
    _of->bytes_tracked+=_og->header_len;
    /*Count the durations of all packets in the page.*/
    do total_duration=op_collect_audio_packets(_of,durations);
    /*Ignore holes.*/
    while(OP_UNLIKELY(total_duration<0));
    op_count=_of->op_count;
  }
  while(op_count<=0);
  /*We found the first page with a completed audio data packet: actually look
     at the granule position.
    RFC 3533 says, "A special value of -1 (in two's complement) indicates that
     no packets finish on this page," which does not say that a granule
     position that is NOT -1 indicates that some packets DO finish on that page
     (even though this was the intention, libogg itself violated this intention
     for years before we fixed it).
    The Ogg Opus specification only imposes its start-time requirements
     on the granule position of the first page with completed packets,
     so we ignore any set granule positions until then.*/
  cur_page_gp=_of->op[op_count-1].granulepos;
  /*But getting a packet without a valid granule position on the page is not
     okay.*/
  if(cur_page_gp==-1)return OP_EBADTIMESTAMP;
  cur_page_eos=_of->op[op_count-1].e_o_s;
  if(OP_LIKELY(!cur_page_eos)){
    /*The EOS flag wasn't set.
      Work backwards from the provided granule position to get the starting PCM
       offset.*/
    if(OP_UNLIKELY(op_granpos_add(&pcm_start,cur_page_gp,-total_duration)<0)){
      /*The starting granule position MUST not be smaller than the amount of
         audio on the first page with completed packets.*/
      return OP_EBADTIMESTAMP;
    }
  }
  else{
    /*The first page with completed packets was also the last.*/
    if(OP_LIKELY(op_granpos_add(&pcm_start,cur_page_gp,-total_duration)<0)){
      /*If there's less audio on the page than indicated by the granule
         position, then we're doing end-trimming, and the starting PCM offset
         is zero by spec mandate.*/
      pcm_start=0;
      /*However, the end-trimming MUST not ask us to trim more samples than
         exist after applying the pre-skip.*/
      if(OP_UNLIKELY(op_granpos_cmp(cur_page_gp,_link->head.pre_skip)<0)){
        return OP_EBADTIMESTAMP;
      }
    }
  }
  /*Timestamp the individual packets.*/
  prev_packet_gp=pcm_start;
  for(pi=0;pi<op_count;pi++){
    int ret;
    if(cur_page_eos){
      ogg_int64_t diff;
      ret=op_granpos_diff(&diff,cur_page_gp,prev_packet_gp);
      OP_ASSERT(!ret);
      diff=durations[pi]-diff;
      /*If we have samples to trim...*/
      if(diff>0){
        /*If we trimmed the entire packet, stop (the spec says encoders
           shouldn't do this, but we support it anyway).*/
        if(OP_UNLIKELY(diff>durations[pi]))break;
        _of->op[pi].granulepos=prev_packet_gp=cur_page_gp;
        /*Move the EOS flag to this packet, if necessary, so we'll trim the
           samples.*/
        _of->op[pi].e_o_s=1;
        continue;
      }
    }
    /*Update the granule position as normal.*/
    ret=op_granpos_add(&_of->op[pi].granulepos,
     prev_packet_gp,durations[pi]);
    OP_ASSERT(!ret);
    prev_packet_gp=_of->op[pi].granulepos;
  }
  /*Update the packet count after end-trimming.*/
  _of->op_count=pi;
  _of->cur_discard_count=_link->head.pre_skip;
  _of->prev_packet_gp=_link->pcm_start=pcm_start;
  return 0;
}

/*Starting from current cursor position, get the final PCM offset of the
   previous page.
  This also validates the duration of the link, which, while not strictly
   required by the spec, we need to ensure duration calculations don't
   overflow.
  This is only done for seekable sources.
  We must validate that op_find_initial_pcm_offset() succeeded for this link
   before calling this function, otherwise it will scan the entire stream
   backwards until it reaches the start, and then fail.*/
static int op_find_final_pcm_offset(OggOpusFile *_of,
 const ogg_uint32_t *_serialnos,int _nserialnos,OggOpusLink *_link,
 ogg_int64_t _end_gp,ogg_uint32_t _end_serialno,ogg_int64_t *_total_duration){
  opus_int64   offset;
  ogg_int64_t  total_duration;
  ogg_int64_t  duration;
  ogg_uint32_t cur_serialno;
  ogg_uint32_t test_serialno;
  opus_int32   chunk_size;
  /*For the time being, fetch end PCM offset the simple way.*/
  cur_serialno=_link->serialno;
  test_serialno=_end_serialno;
  /*Keep track of the growing chunk size to better handle being multiplexed
     with another high-bitrate stream.*/
  chunk_size=OP_CHUNK_SIZE;
  offset=_of->offset;
  while(_end_gp==-1||test_serialno!=cur_serialno){
    test_serialno=cur_serialno;
    _of->offset=offset;
    offset=op_get_prev_page_serial(_of,_serialnos,_nserialnos,
     &chunk_size,&test_serialno,&_end_gp);
    if(OP_UNLIKELY(offset<0))return (int)offset;
  }
  /*This implementation requires that difference between the first and last
     granule positions in each link be representable in a signed, 64-bit
     number, and that each link also have at least as many samples as the
     pre-skip requires.*/
  if(OP_UNLIKELY(op_granpos_diff(&duration,_end_gp,_link->pcm_start)<0)
   ||OP_UNLIKELY(duration<_link->head.pre_skip)){
    return OP_EBADTIMESTAMP;
  }
  /*We also require that the total duration be representable in a signed,
     64-bit number.*/
  duration-=_link->head.pre_skip;
  total_duration=*_total_duration;
  if(OP_UNLIKELY(OP_INT64_MAX-duration<total_duration))return OP_EBADTIMESTAMP;
  *_total_duration=total_duration+duration;
  _link->pcm_end=_end_gp;
  _link->end_offset=offset;
  return 0;
}

typedef struct op_seek_record op_seek_record;

/*We use this to remember the pages we found while enumerating the links of a
   chained stream.
  We only need to know the starting and ending byte offsets and the serial
   number, so we can tell if the page belonged to the current chain or not,
   and where to bisect.*/
struct op_seek_record{
  opus_int64   offset;
  opus_int32   size;
  ogg_uint32_t serialno;
};

/*Finds each bitstream link, one at a time, using a bisection search.
  This has to begin by knowing the offset of the first link's initial page.*/
static int op_bisect_forward_serialno(OggOpusFile *_of,
 opus_int64 _searched,ogg_int64_t _end_gp,op_seek_record *_sr,int _csr,
 ogg_uint32_t **_serialnos,int *_nserialnos,int *_cserialnos){
  ogg_page      og;
  OggOpusLink  *links;
  int           nlinks;
  int           clinks;
  ogg_uint32_t *serialnos;
  int           nserialnos;
  opus_int64    begin;
  ogg_int64_t   total_duration;
  int           nsr;
  int           ret;
  links=_of->links;
  nlinks=clinks=_of->nlinks;
  begin=0;
  total_duration=0;
  /*We start with one seek record, for the last page in the file.
    We build up a list of records for places we seek to during link
     enumeration.
    This list is kept sorted in reverse order.
    We only care about seek locations that were _not_ in the current link,
     therefore we can add them one at a time to the end of the list as we
     improve the lower bound on the location where the next link starts.*/
  nsr=1;
  for(;;){
    opus_int64 data_offset;
    opus_int64 end_searched;
    opus_int64 next;
    opus_int64 last;
    int        sri;
    serialnos=*_serialnos;
    nserialnos=*_nserialnos;
    if(OP_UNLIKELY(nlinks>=clinks)){
      if(OP_UNLIKELY(clinks>INT_MAX-1>>1))return OP_EFAULT;
      clinks=2*clinks+1;
      OP_ASSERT(nlinks<clinks);
      links=_ogg_realloc(links,sizeof(*links)*clinks);
      if(OP_UNLIKELY(links==NULL))return OP_EFAULT;
      _of->links=links;
    }
    data_offset=_searched;
    /*Invariants:
      We have the headers and serial numbers for the link beginning at 'begin'.
      We have the offset and granule position of the last page in the file
       (potentially not a page we care about).*/
    /*Scan the seek records we already have to save us some bisection.*/
    for(sri=0;sri<nsr;sri++){
      if(op_lookup_serialno(_sr[sri].serialno,*_serialnos,*_nserialnos))break;
    }
    /*Is the last page in our current list of serial numbers?*/
    if(sri<=0)break;
    /*Last page wasn't found.
      We have at least one more link.*/
    end_searched=next=_sr[sri-1].offset;
    if(sri<nsr)_searched=_sr[sri].offset+_sr[sri].size;
    nsr=sri;
    /*We guard against garbage separating the last and first pages of two
       links below.*/
    while(_searched<end_searched){
      opus_int64 bisect;
      if(OP_UNLIKELY(end_searched-_searched<OP_CHUNK_SIZE))bisect=_searched;
      /*TODO: We might be able to do a better job estimating the start of
         subsequent links by assuming its initial PCM offset is 0 and using two
         sightings of the same stream to estimate a bitrate.*/
      else bisect=_searched+(end_searched-_searched>>1);
      if(OP_LIKELY(bisect!=_of->offset)){
        ret=op_seek_helper(_of,bisect);
        if(OP_UNLIKELY(ret<0))return ret;
      }
      last=op_get_next_page(_of,&og,-1);
      /*At the worst we should have hit the page at _sr[sri-1].offset.*/
      if(OP_UNLIKELY(last<0))return OP_EBADLINK;
      OP_ASSERT(nsr<_csr);
      _sr[nsr].serialno=ogg_page_serialno(&og);
      if(!op_lookup_serialno(_sr[nsr].serialno,serialnos,nserialnos)){
        end_searched=bisect;
        next=last;
        /*In reality we should always have enough room, but be paranoid.*/
        if(OP_LIKELY(nsr+1<_csr)){
          _sr[nsr].offset=last;
          OP_ASSERT(_of->offset-last>=0);
          OP_ASSERT(_of->offset-last<=OP_PAGE_SIZE);
          _sr[nsr].size=(opus_int32)(_of->offset-last);
          nsr++;
        }
      }
      else _searched=_of->offset;
    }
    /*Bisection point found.
      Get the final granule position of the previous link, assuming
       op_find_initial_pcm_offset() didn't already determine the link was
       empty.*/
    if(OP_LIKELY(links[nlinks-1].pcm_end==-1)){
      _of->offset=next;
      ret=op_find_final_pcm_offset(_of,serialnos,nserialnos,
       links+nlinks-1,-1,0,&total_duration);
      if(OP_UNLIKELY(ret<0))return ret;
    }
    /*Restore the cursor position after the seek.
      This should only be necessary if the last page in the link did not belong
       to our Opus stream.
      TODO: Read forward instead, or let seek implementations do that?*/
    if(_of->offset!=next){
      ret=op_seek_helper(_of,next);
      if(OP_UNLIKELY(ret<0))return ret;
    }
    ret=op_fetch_headers(_of,&links[nlinks].head,&links[nlinks].tags,
     _serialnos,_nserialnos,_cserialnos,NULL);
    if(OP_UNLIKELY(ret<0))return ret;
    links[nlinks].offset=next;
    links[nlinks].data_offset=_of->offset;
    links[nlinks].serialno=_of->os.serialno;
    links[nlinks].pcm_end=-1;
    /*This might consume a page from the next link, however the next bisection
       always starts with a seek.*/
    ret=op_find_initial_pcm_offset(_of,links+nlinks,NULL);
    if(OP_UNLIKELY(ret<0))return ret;
    begin=next;
    _searched=_of->offset;
    /*Mark the current link count so it can be cleaned up on error.*/
    _of->nlinks=++nlinks;
  }
  /*Last page is in the starting serialno list, so we've reached the last link.
    Now find the last granule position for it (if we didn't the first time we
     looked at the end of the stream, and if op_find_initial_pcm_offset()
     didn't already determine the link was empty).*/
  if(OP_LIKELY(links[nlinks-1].pcm_end==-1)){
    _of->offset=_sr[0].offset;
    ret=op_find_final_pcm_offset(_of,serialnos,nserialnos,
     links+nlinks-1,_end_gp,_sr[0].serialno,&total_duration);
    if(OP_UNLIKELY(ret<0))return ret;
  }
  /*Trim back the links array if necessary.*/
  links=_ogg_realloc(links,sizeof(*links)*nlinks);
  if(OP_LIKELY(links!=NULL))_of->links=links;
  /*We also don't need these anymore.*/
  _ogg_free(*_serialnos);
  *_serialnos=NULL;
  *_cserialnos=*_nserialnos=0;
  return 0;
}

static int op_make_decode_ready(OggOpusFile *_of){
  OpusHead *head;
  int       li;
  int       stream_count;
  int       coupled_count;
  int       channel_count;
  if(_of->ready_state>OP_STREAMSET)return 0;
  if(OP_UNLIKELY(_of->ready_state<OP_STREAMSET))return OP_EFAULT;
  li=_of->seekable?_of->cur_link:0;
  head=&_of->links[li].head;
  stream_count=head->stream_count;
  coupled_count=head->coupled_count;
  channel_count=head->channel_count;
  /*Check to see if the current decoder is compatible with the current link.*/
  if(_of->od!=NULL&&_of->od_stream_count==stream_count
   &&_of->od_coupled_count==coupled_count&&_of->od_channel_count==channel_count
   &&memcmp(_of->od_mapping,head->mapping,
   sizeof(*head->mapping)*channel_count)==0){
    opus_multistream_decoder_ctl(_of->od,OPUS_RESET_STATE);
  }
  else{
    int err;
    opus_multistream_decoder_destroy(_of->od);
    _of->od=opus_multistream_decoder_create(48000,channel_count,
     stream_count,coupled_count,head->mapping,&err);
    if(_of->od==NULL)return OP_EFAULT;
    _of->od_stream_count=stream_count;
    _of->od_coupled_count=coupled_count;
    _of->od_channel_count=channel_count;
    memcpy(_of->od_mapping,head->mapping,sizeof(*head->mapping)*channel_count);
  }
  /*TODO: Implement this when not available, or require sufficiently new
     libopus?*/
#if defined(OPUS_SET_GAIN)
  opus_multistream_decoder_ctl(_of->od,OPUS_SET_GAIN(head->output_gain));
#endif
  _of->ready_state=OP_INITSET;
  _of->bytes_tracked=0;
  _of->samples_tracked=0;
#if !defined(OP_FIXED_POINT)
  _of->dither_mute=65;
  /*Use the serial number for the PRNG seed to get repeatable output for
     straight play-throughs.*/
  _of->dither_seed=_of->links[li].serialno;
#endif
  return 0;
}

static int op_open_seekable2(OggOpusFile *_of){
  /*64 seek records should be enough for anybody.
    Actually, with a bisection search in a 63-bit range down to OP_CHUNK_SIZE
     granularity, much more than enough.*/
  op_seek_record sr[64];
  opus_int64     data_offset;
  ogg_int64_t    end_gp;
  int            ret;
  /*We're partially open and have a first link header state in storage in _of.*/
  /*We can seek, so set out learning all about this file.*/
  (*_of->callbacks.seek)(_of->source,0,SEEK_END);
  _of->offset=_of->end=(*_of->callbacks.tell)(_of->source);
  /*Get the offset of the last page of the physical bitstream, or, if we're
     lucky, the last Opus page of the first link, as most Ogg Opus files will
     contain a single logical bitstream.*/
  sr[0].serialno=_of->links[0].serialno;
  sr[0].offset=op_get_prev_page_serial(_of,_of->serialnos,_of->nserialnos,
   NULL,&sr[0].serialno,&end_gp);
  if(OP_UNLIKELY(sr[0].offset<0))return (int)sr[0].offset;
  /*Now enumerate the bitstream structure.*/
  OP_ASSERT(_of->offset-sr[0].offset>=0);
  OP_ASSERT(_of->offset-sr[0].offset<=OP_PAGE_SIZE);
  sr[0].size=(opus_int32)(_of->offset-sr[0].offset);
  data_offset=_of->links[0].data_offset;
  ret=op_bisect_forward_serialno(_of,data_offset,end_gp,sr,
   sizeof(sr)/sizeof(*sr),&_of->serialnos,&_of->nserialnos,&_of->cserialnos);
  if(OP_UNLIKELY(ret<0))return ret;
  /*And seek back to the start of the first link.*/
  return op_raw_seek(_of,data_offset);
}

/*Clear out the current logical bitstream decoder.*/
static void op_decode_clear(OggOpusFile *_of){
  /*We don't actually free the decoder.
    We might be able to re-use it for the next link.*/
  _of->op_count=0;
  _of->od_buffer_size=0;
  _of->prev_packet_gp=-1;
  if(!_of->seekable){
    OP_ASSERT(_of->ready_state>=OP_INITSET);
    opus_tags_clear(&_of->links[0].tags);
  }
  _of->ready_state=OP_OPENED;
}

static void op_clear(OggOpusFile *_of){
  OggOpusLink *links;
  _ogg_free(_of->od_buffer);
  if(_of->od!=NULL)opus_multistream_decoder_destroy(_of->od);
  links=_of->links;
  if(!_of->seekable){
    if(_of->ready_state>OP_OPENED)opus_tags_clear(&links[0].tags);
  }
  else if(OP_LIKELY(links!=NULL)){
    int nlinks;
    int link;
    nlinks=_of->nlinks;
    for(link=0;link<nlinks;link++)opus_tags_clear(&links[link].tags);
  }
  _ogg_free(links);
  _ogg_free(_of->serialnos);
  ogg_stream_clear(&_of->os);
  ogg_sync_clear(&_of->oy);
  if(_of->callbacks.close!=NULL)(*_of->callbacks.close)(_of->source);
}

static int op_open1(OggOpusFile *_of,
 void *_source,const OpusFileCallbacks *_cb,
 const unsigned char *_initial_data,size_t _initial_bytes){
  ogg_page  og;
  ogg_page *pog;
  int       seekable;
  int       ret;
  memset(_of,0,sizeof(*_of));
  _of->source=_source;
  *&_of->callbacks=*_cb;
  /*At a minimum, we need to be able to read data.*/
  if(OP_UNLIKELY(_of->callbacks.read==NULL))return OP_EREAD;
  /*Initialize the framing state.*/
  ogg_sync_init(&_of->oy);
  /*Perhaps some data was previously read into a buffer for testing against
     other stream types.
    Allow initialization from this previously read data (especially as we may
     be reading from a non-seekable stream).
    This requires copying it into a buffer allocated by ogg_sync_buffer() and
     doesn't support seeking, so this is not a good mechanism to use for
     decoding entire files from RAM.*/
  if(_initial_bytes>0){
    char *buffer;
    buffer=ogg_sync_buffer(&_of->oy,_initial_bytes);
    memcpy(buffer,_initial_data,_initial_bytes*sizeof(*buffer));
    ogg_sync_wrote(&_of->oy,_initial_bytes);
  }
  /*Can we seek?
    Stevens suggests the seek test is portable.*/
  seekable=_cb->seek!=NULL&&(*_cb->seek)(_source,0,SEEK_CUR)!=-1;
  /*If seek is implemented, tell must also be implemented.*/
  if(seekable){
    if(OP_UNLIKELY(_of->callbacks.tell==NULL))return OP_EINVAL;
    else{
      opus_int64 pos;
      pos=(*_of->callbacks.tell)(_of->source);
      /*If the current position is not equal to the initial bytes consumed,
         absolute seeking will not work.*/
      if(OP_UNLIKELY(pos!=(opus_int64)_initial_bytes))return OP_EINVAL;
    }
  }
  _of->seekable=seekable;
  /*Don't seek yet.
    Set up a 'single' (current) logical bitstream entry for partial open.*/
  _of->nlinks=1;
  _of->links=(OggOpusLink *)_ogg_malloc(sizeof(*_of->links));
  /*The serialno gets filled in later by op_fetch_headers().*/
  ogg_stream_init(&_of->os,-1);
  pog=NULL;
  for(;;){
    /*Fetch all BOS pages, store the Opus header and all seen serial numbers,
      and load subsequent Opus setup headers.*/
    ret=op_fetch_headers(_of,&_of->links[0].head,&_of->links[0].tags,
     &_of->serialnos,&_of->nserialnos,&_of->cserialnos,pog);
    if(OP_UNLIKELY(ret<0))break;
    _of->links[0].offset=0;
    _of->links[0].data_offset=_of->offset;
    _of->links[0].pcm_end=-1;
    _of->links[0].serialno=_of->os.serialno;
    /*Fetch the initial PCM offset.*/
    ret=op_find_initial_pcm_offset(_of,_of->links,&og);
    if(seekable||OP_LIKELY(ret<=0))break;
    /*This link was empty, but we already have the BOS page for the next one in
       og.
      We can't seek, so start processing the next link right now.*/
    pog=&og;
    _of->cur_link++;
  }
  if(OP_UNLIKELY(ret<0)){
    /*Don't auto-close the stream on failure.*/
    _of->callbacks.close=NULL;
    op_clear(_of);
  }
  else _of->ready_state=OP_PARTOPEN;
  return ret;
}

static int op_open2(OggOpusFile *_of){
  int ret;
  OP_ASSERT(_of->ready_state==OP_PARTOPEN);
  if(_of->seekable){
    _of->ready_state=OP_OPENED;
    ret=op_open_seekable2(_of);
  }
  else{
    /*We have buffered packets from op_find_initial_pcm_offset().
      Move to OP_INITSET so we can use them.*/
    _of->ready_state=OP_STREAMSET;
    ret=op_make_decode_ready(_of);
  }
  if(OP_UNLIKELY(ret<0)){
    /*Don't auto-close the stream on failure.*/
    _of->callbacks.close=NULL;
    op_clear(_of);
    return ret;
  }
  return 0;
}

OggOpusFile *op_test_callbacks(void *_source,const OpusFileCallbacks *_cb,
 const unsigned char *_initial_data,size_t _initial_bytes,int *_error){
  OggOpusFile *of;
  int          ret;
  of=(OggOpusFile *)_ogg_malloc(sizeof(*of));
  ret=OP_EFAULT;
  if(OP_LIKELY(of!=NULL)){
    ret=op_open1(of,_source,_cb,_initial_data,_initial_bytes);
    if(OP_LIKELY(ret>=0)){
      if(_error!=NULL)*_error=0;
      return of;
    }
    _ogg_free(of);
  }
  if(_error!=NULL)*_error=ret;
  return NULL;
}

OggOpusFile *op_open_callbacks(void *_source,const OpusFileCallbacks *_cb,
 const unsigned char *_initial_data,size_t _initial_bytes,int *_error){
  OggOpusFile *of;
  of=op_test_callbacks(_source,_cb,_initial_data,_initial_bytes,_error);
  if(OP_LIKELY(of!=NULL)){
    int ret;
    ret=op_open2(of);
    if(OP_LIKELY(ret>=0))return of;
    if(_error!=NULL)*_error=ret;
    _ogg_free(of);
  }
  return NULL;
}

/*Convenience routine to clean up from failure for the open functions that
   create their own streams.*/
static OggOpusFile *op_open_close_on_failure(void *_source,
 const OpusFileCallbacks *_cb,int *_error){
  OggOpusFile *of;
  if(OP_UNLIKELY(_source==NULL)){
    if(_error!=NULL)*_error=OP_EFAULT;
    return NULL;
  }
  of=op_open_callbacks(_source,_cb,NULL,0,_error);
  if(OP_UNLIKELY(of==NULL))(*_cb->close)(_source);
  return of;
}

OggOpusFile *op_open_file(const char *_path,int *_error){
  OpusFileCallbacks cb;
  return op_open_close_on_failure(op_fopen(&cb,_path,"rb"),&cb,_error);
}

OggOpusFile *op_open_memory(const unsigned char *_data,size_t _size,
 int *_error){
  OpusFileCallbacks cb;
  return op_open_close_on_failure(op_mem_stream_create(&cb,_data,_size),&cb,
   _error);
}

/*Convenience routine to clean up from failure for the open functions that
   create their own streams.*/
static OggOpusFile *op_test_close_on_failure(void *_source,
 const OpusFileCallbacks *_cb,int *_error){
  OggOpusFile *of;
  if(OP_UNLIKELY(_source==NULL)){
    if(_error!=NULL)*_error=OP_EFAULT;
    return NULL;
  }
  of=op_test_callbacks(_source,_cb,NULL,0,_error);
  if(OP_UNLIKELY(of==NULL))(*_cb->close)(_source);
  return of;
}

OggOpusFile *op_test_file(const char *_path,int *_error){
  OpusFileCallbacks cb;
  return op_test_close_on_failure(op_fopen(&cb,_path,"rb"),&cb,_error);
}

OggOpusFile *op_test_memory(const unsigned char *_data,size_t _size,
 int *_error){
  OpusFileCallbacks cb;
  return op_test_close_on_failure(op_mem_stream_create(&cb,_data,_size),&cb,
   _error);
}

int op_test_open(OggOpusFile *_of){
  int ret;
  if(OP_UNLIKELY(_of->ready_state!=OP_PARTOPEN))return OP_EINVAL;
  ret=op_open2(_of);
  /*op_open2() will clear this structure on failure.
    Reset its contents to prevent double-frees in op_free().*/
  if(OP_UNLIKELY(ret<0))memset(_of,0,sizeof(*_of));
  return ret;
}

void op_free(OggOpusFile *_of){
  if(OP_LIKELY(_of!=NULL)){
    op_clear(_of);
    _ogg_free(_of);
  }
}

int op_link_count(OggOpusFile *_of){
  return _of->nlinks;
}

int op_seekable(OggOpusFile *_of){
  return _of->seekable;
}

ogg_uint32_t op_serialno(OggOpusFile *_of,int _li){
  if(OP_UNLIKELY(_li>=_of->nlinks))_li=_of->nlinks-1;
  if(!_of->seekable&&_li!=0)_li=0;
  return _of->links[_li<0?_of->cur_link:_li].serialno;
}

int op_channel_count(OggOpusFile *_of,int _li){
  if(OP_UNLIKELY(_li>=_of->nlinks))_li=_of->nlinks-1;
  if(!_of->seekable&&_li!=0)_li=0;
  return _of->links[_li<0?_of->cur_link:_li].head.channel_count;
}

opus_int64 op_raw_total(OggOpusFile *_of,int _li){
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED)
   ||OP_UNLIKELY(!_of->seekable)
   ||OP_UNLIKELY(_li>=_of->nlinks)){
    return OP_EINVAL;
  }
  if(_li<0)return _of->end-_of->links[0].offset;
  return (_li+1>=_of->nlinks?_of->end:_of->links[_li+1].offset)
   -_of->links[_li].offset;
}

ogg_int64_t op_pcm_total(OggOpusFile *_of,int _li){
  OggOpusLink *links;
  ogg_int64_t  diff;
  int          ret;
  int          nlinks;
  nlinks=_of->nlinks;
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED)
   ||OP_UNLIKELY(!_of->seekable)
   ||OP_UNLIKELY(_li>=nlinks)){
    return OP_EINVAL;
  }
  links=_of->links;
  /*We verify that the granule position differences are larger than the
     pre-skip and that the total duration does not overflow during link
     enumeration, so we don't have to check here.*/
  if(_li<0){
    ogg_int64_t pcm_total;
    int         li;
    pcm_total=0;
    for(li=0;li<nlinks;li++){
      ret=op_granpos_diff(&diff,links[li].pcm_end,links[li].pcm_start);
      OP_ASSERT(!ret);
      pcm_total+=diff-links[li].head.pre_skip;
    }
    return pcm_total;
  }
  ret=op_granpos_diff(&diff,links[_li].pcm_end,links[_li].pcm_start);
  OP_ASSERT(!ret);
  return diff-links[_li].head.pre_skip;
}

const OpusHead *op_head(OggOpusFile *_of,int _li){
  if(!_of->seekable)_li=0;
  else if(_li<0)_li=_of->ready_state>=OP_STREAMSET?_of->cur_link:0;
  return _li>=_of->nlinks?NULL:&_of->links[_li].head;
}

const OpusTags *op_tags(OggOpusFile *_of,int _li){
  if(!_of->seekable)_li=0;
  else if(_li<0)_li=_of->ready_state>=OP_STREAMSET?_of->cur_link:0;
  return _li>=_of->nlinks?NULL:&_of->links[_li].tags;
}

/*Compute an average bitrate given a byte and sample count.
  Return: The bitrate in bits per second.*/
static opus_int32 op_calc_bitrate(opus_int64 _bytes,ogg_int64_t _samples){
  /*These rates are absurd, but let's handle them anyway.*/
  if(OP_UNLIKELY(_bytes>(OP_INT64_MAX-(_samples>>1))/(48000*8))){
    ogg_int64_t den;
    if(OP_UNLIKELY(_bytes/(0x7FFFFFFFF/(48000*8))>=_samples))return 0x7FFFFFFF;
    den=_samples/(48000*8);
    return (_bytes+(den>>1))/den;
  }
  if(OP_UNLIKELY(_samples<=0))return 0x7FFFFFFF;
  /*This can't actually overflow in normal operation: even with a pre-skip of
     545 2.5 ms frames with 8 streams running at 1282*8+1 bytes per packet
     (1275 byte frames + Opus framing overhead + Ogg lacing values), that all
     produce a single sample of decoded output, we still don't top 45 Mbps.
    The only way to get bitrates larger than that is with excessive Opus
     padding, more encoded streams than output channels, or lots and lots of
     Ogg pages with no packets on them.*/
  return (opus_int32)OP_MIN((_bytes*48000*8+(_samples>>1))/_samples,0x7FFFFFFF);
}

opus_int32 op_bitrate(OggOpusFile *_of,int _li){
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED)||OP_UNLIKELY(!_of->seekable)
   ||OP_UNLIKELY(_li>=_of->nlinks)){
    return OP_EINVAL;
  }
  return op_calc_bitrate(op_raw_total(_of,_li),op_pcm_total(_of,_li));
}

opus_int32 op_bitrate_instant(OggOpusFile *_of){
  ogg_int64_t samples_tracked;
  opus_int32  ret;
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED))return OP_EINVAL;
  samples_tracked=_of->samples_tracked;
  if(OP_UNLIKELY(samples_tracked==0))return OP_FALSE;
  ret=op_calc_bitrate(_of->bytes_tracked,samples_tracked);
  _of->bytes_tracked=0;
  _of->samples_tracked=0;
  return ret;
}

/*Fetch and process a page.
  This handles the case where we're at a bitstream boundary and dumps the
   decoding machine.
  If the decoding machine is unloaded, it loads it.
  It also keeps prev_packet_gp up to date (seek and read both use this; seek
   uses a special hack with _readp).
  Return: <0) Error, OP_HOLE (lost packet), or OP_EOF.
           0) Need more data (only if _readp==0).
           1) Got at least one audio data packet.*/
static int op_fetch_and_process_page(OggOpusFile *_of,int _readp,int _spanp){
  OggOpusLink  *links;
  ogg_uint32_t  cur_serialno;
  int           seekable;
  int           cur_link;
  int           ret;
  if(OP_LIKELY(_of->ready_state>=OP_INITSET)
   &&OP_LIKELY(_of->op_pos<_of->op_count)){
    /*We're ready to decode and have at least one packet available already.*/
    return 1;
  }
  if(!_readp)return 0;
  seekable=_of->seekable;
  links=_of->links;
  cur_link=seekable?_of->cur_link:0;
  cur_serialno=links[cur_link].serialno;
  /*Handle one page.*/
  for(;;){
    ogg_page   og;
    opus_int64 page_pos;
    OP_ASSERT(_of->ready_state>=OP_OPENED);
    /*This loop is not strictly necessary, but there's no sense in doing the
       extra checks of the larger loop for the common case in a multiplexed
       bistream where the page is simply part of a different logical
       bitstream.*/
    do{
      /*Keep reading until we get a page with the correct serialno.*/
      page_pos=op_get_next_page(_of,&og,-1);
      /*EOF: Leave uninitialized.*/
      if(page_pos<0)return OP_EOF;
      if(OP_LIKELY(_of->ready_state>=OP_STREAMSET)){
        if(cur_serialno!=(ogg_uint32_t)ogg_page_serialno(&og)){
          /*Two possibilities:
             1) Another stream is multiplexed into this logical section, or*/
          if(OP_LIKELY(!ogg_page_bos(&og)))continue;
          /* 2) Our decoding just traversed a bitstream boundary.*/
          if(!_spanp)return OP_EOF;
          if(OP_LIKELY(_of->ready_state>=OP_INITSET))op_decode_clear(_of);
          break;
        }
      }
      /*Bitrate tracking: add the header's bytes here.
        The body bytes are counted when we consume the packets.*/
      _of->bytes_tracked+=og.header_len;
    }
    while(0);
    /*Do we need to load a new machine before submitting the page?
      This is different in the seekable and non-seekable cases.
      In the seekable case, we already have all the header information loaded
       and cached.
      We just initialize the machine with it and continue on our merry way.
      In the non-seekable (streaming) case, we'll only be at a boundary if we
       just left the previous logical bitstream, and we're now nominally at the
       header of the next bitstream.*/
    if(OP_UNLIKELY(_of->ready_state<OP_STREAMSET)){
      if(seekable){
        ogg_uint32_t serialno;
        int          nlinks;
        int          li;
        serialno=ogg_page_serialno(&og);
        /*Match the serialno to bitstream section.
          We use this rather than offset positions to avoid problems near
           logical bitstream boundaries.*/
        nlinks=_of->nlinks;
        for(li=0;li<nlinks&&links[li].serialno!=serialno;li++);
        /*Not a desired Opus bitstream section.
          Keep trying.*/
        if(li>=nlinks)continue;
        cur_serialno=serialno;
        _of->cur_link=cur_link=li;
        ogg_stream_reset_serialno(&_of->os,serialno);
        _of->ready_state=OP_STREAMSET;
        /*If we're at the start of this link, initialize the granule position
           and pre-skip tracking.*/
        if(page_pos<=links[cur_link].data_offset){
          _of->prev_packet_gp=links[cur_link].pcm_start;
          _of->cur_discard_count=links[cur_link].head.pre_skip;
        }
      }
      else{
        do{
          /*We're streaming.
            Fetch the two header packets, build the info struct.*/
          ret=op_fetch_headers(_of,&links[0].head,&links[0].tags,
           NULL,NULL,NULL,&og);
          if(OP_UNLIKELY(ret<0))return ret;
          ret=op_find_initial_pcm_offset(_of,links,&og);
          if(OP_UNLIKELY(ret<0))return ret;
          _of->links[0].serialno=cur_serialno=_of->os.serialno;
          _of->cur_link++;
        }
        /*If the link was empty, keep going, because we already have the
           BOS page of the next one in og.*/
        while(OP_UNLIKELY(ret>0));
        /*If we didn't get any packets out of op_find_initial_pcm_offset(),
           keep going (this is possible if end-trimming trimmed them all).*/
        if(_of->op_count<=0)continue;
        /*Otherwise, we're done.*/
        ret=op_make_decode_ready(_of);
        if(OP_UNLIKELY(ret<0))return ret;
        return 1;
      }
    }
    /*The buffered page is the data we want, and we're ready for it.
      Add it to the stream state.*/
    if(OP_UNLIKELY(_of->ready_state==OP_STREAMSET)){
      ret=op_make_decode_ready(_of);
      if(OP_UNLIKELY(ret<0))return ret;
    }
    /*Extract all the packets from the current page.*/
    ogg_stream_pagein(&_of->os,&og);
    if(OP_LIKELY(_of->ready_state>=OP_INITSET)){
      opus_int32 total_duration;
      int        durations[255];
      int        op_count;
      total_duration=op_collect_audio_packets(_of,durations);
      /*Report holes to the caller.*/
      if(OP_UNLIKELY(total_duration<0))return (int)total_duration;
      op_count=_of->op_count;
      /*If we found at least one audio data packet, compute per-packet granule
         positions for them.*/
      if(op_count>0){
        ogg_int64_t diff;
        ogg_int64_t prev_packet_gp;
        ogg_int64_t cur_packet_gp;
        ogg_int64_t cur_page_gp;
        int         cur_page_eos;
        int         pi;
        cur_page_gp=_of->op[op_count-1].granulepos;
        cur_page_eos=_of->op[op_count-1].e_o_s;
        prev_packet_gp=_of->prev_packet_gp;
        if(OP_UNLIKELY(prev_packet_gp==-1)){
          opus_int32 cur_discard_count;
          /*This is the first call after a raw seek.
            Try to reconstruct prev_packet_gp from scratch.*/
          OP_ASSERT(seekable);
          if(OP_UNLIKELY(cur_page_eos)){
            /*If the first page we hit after our seek was the EOS page, and
               we didn't start from data_offset or before, we don't have
               enough information to do end-trimming.
              Proceed to the next link, rather than risk playing back some
               samples that shouldn't have been played.*/
            _of->op_count=0;
            continue;
          }
          /*By default discard 80 ms of data after a seek, unless we seek
             into the pre-skip region.*/
          cur_discard_count=80*48;
          cur_page_gp=_of->op[op_count-1].granulepos;
          /*Try to initialize prev_packet_gp.
            If the current page had packets but didn't have a granule
             position, or the granule position it had was too small (both
             illegal), just use the starting granule position for the link.*/
          prev_packet_gp=links[cur_link].pcm_start;
          if(OP_LIKELY(cur_page_gp!=-1)){
            op_granpos_add(&prev_packet_gp,cur_page_gp,-total_duration);
          }
          if(OP_LIKELY(!op_granpos_diff(&diff,
           prev_packet_gp,links[cur_link].pcm_start))){
            opus_int32 pre_skip;
            /*If we start at the beginning of the pre-skip region, or we're
               at least 80 ms from the end of the pre-skip region, we discard
               to the end of the pre-skip region.
              Otherwise, we still use the 80 ms default, which will discard
               past the end of the pre-skip region.*/
            pre_skip=links[cur_link].head.pre_skip;
            if(diff>=0&&diff<=OP_MAX(0,pre_skip-80*48)){
              cur_discard_count=pre_skip-(int)diff;
            }
          }
          _of->cur_discard_count=cur_discard_count;
        }
        if(OP_UNLIKELY(cur_page_gp==-1)){
          /*This page had completed packets but didn't have a valid granule
             position.
            This is illegal, but we'll try to handle it by continuing to count
             forwards from the previous page.*/
          if(op_granpos_add(&cur_page_gp,prev_packet_gp,total_duration)<0){
            /*The timestamp for this page overflowed.*/
            cur_page_gp=links[cur_link].pcm_end;
          }
        }
        /*If we hit the last page, handle end-trimming.*/
        if(OP_UNLIKELY(cur_page_eos)
         &&OP_LIKELY(!op_granpos_diff(&diff,cur_page_gp,prev_packet_gp))
         &&OP_LIKELY(diff<total_duration)){
          cur_packet_gp=prev_packet_gp;
          for(pi=0;pi<op_count;pi++){
            diff=durations[pi]-diff;
            /*If we have samples to trim...*/
            if(diff>0){
              /*If we trimmed the entire packet, stop (the spec says encoders
                 shouldn't do this, but we support it anyway).*/
              if(OP_UNLIKELY(diff>durations[pi]))break;
              cur_packet_gp=cur_page_gp;
              /*Move the EOS flag to this packet, if necessary, so we'll trim
                 the samples during decode.*/
              _of->op[pi].e_o_s=1;
            }
            else{
              /*Update the granule position as normal.*/
              ret=op_granpos_add(&cur_packet_gp,cur_packet_gp,durations[pi]);
              OP_ASSERT(!ret);
            }
            _of->op[pi].granulepos=cur_packet_gp;
            ret=op_granpos_diff(&diff,cur_page_gp,cur_packet_gp);
            OP_ASSERT(!ret);
          }
        }
        else{
          /*Propagate timestamps to earlier packets.
            op_granpos_add(&prev_packet_gp,prev_packet_gp,total_duration)
             should succeed and give prev_packet_gp==cur_page_gp.
            But we don't bother to check that, as there isn't much we can do
             if it's not true.
            The only thing we guarantee is that the start and end granule
             positions of the packets are valid, and that they are monotonic
             within a page.
            They might be completely out of range for this link (we'll check
             that elsewhere), or non-monotonic between pages.*/
          if(OP_UNLIKELY(op_granpos_add(&prev_packet_gp,
           cur_page_gp,-total_duration)<0)){
            /*The starting timestamp for the first packet on this page
               underflowed.
              This is illegal, but we ignore it.*/
            prev_packet_gp=0;
          }
          for(pi=0;pi<op_count;pi++){
            if(OP_UNLIKELY(op_granpos_add(&cur_packet_gp,
             cur_page_gp,-total_duration)<0)){
              /*The start timestamp for this packet underflowed.
                This is illegal, but we ignore it.*/
              cur_packet_gp=0;
            }
            total_duration-=durations[pi];
            OP_ASSERT(total_duration>=0);
            ret=op_granpos_add(&cur_packet_gp,cur_packet_gp,durations[pi]);
            OP_ASSERT(!ret);
            _of->op[pi].granulepos=cur_packet_gp;
          }
          OP_ASSERT(total_duration==0);
        }
        _of->prev_packet_gp=prev_packet_gp;
        _of->op_count=pi;
        /*If end-trimming didn't trim all the packets, we're done.*/
        if(OP_LIKELY(pi>0))return 1;
      }
    }
  }
}

int op_raw_seek(OggOpusFile *_of,opus_int64 _pos){
  OggOpusLink *links;
  int          nlinks;
  int          cur_link;
  int          ret;
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED))return OP_EINVAL;
  /*Don't dump the decoder state if we can't seek.*/
  if(OP_UNLIKELY(!_of->seekable))return OP_ENOSEEK;
  if(OP_UNLIKELY(_pos<0)||OP_UNLIKELY(_pos>_of->end))return OP_EINVAL;
  links=_of->links;
  nlinks=_of->nlinks;
  cur_link=_of->cur_link;
  /*Clear out any buffered, decoded data.*/
  op_decode_clear(_of);
  _of->bytes_tracked=0;
  _of->samples_tracked=0;
  ret=op_seek_helper(_of,_pos);
  if(OP_UNLIKELY(ret<0))return OP_EREAD;
  do ret=op_fetch_and_process_page(_of,1,1);
  /*Ignore holes.*/
  while(ret==OP_HOLE);
  /*If we hit EOF, op_fetch_and_process_page() leaves us uninitialized.
    Instead, jump to the end.*/
  if(ret==OP_EOF){
    cur_link=nlinks-1;
    op_decode_clear(_of);
    _of->cur_link=cur_link;
    _of->prev_packet_gp=links[cur_link].pcm_end;
    _of->cur_discard_count=0;
    ret=0;
  }
  else if(ret>0)ret=0;
  return ret;
}

/*Convert a PCM offset relative to the start of the whole stream to a granule
   position in an individual link.*/
static ogg_int64_t op_get_granulepos(const OggOpusFile *_of,
 ogg_int64_t _pcm_offset,int *_li){
  OggOpusLink *links;
  ogg_int64_t  duration;
  int          nlinks;
  int          li;
  int          ret;
  OP_ASSERT(_pcm_offset>=0);
  nlinks=_of->nlinks;
  links=_of->links;
  for(li=0;OP_LIKELY(li<nlinks);li++){
    ogg_int64_t pcm_start;
    opus_int32  pre_skip;
    pcm_start=links[li].pcm_start;
    pre_skip=links[li].head.pre_skip;
    ret=op_granpos_diff(&duration,links[li].pcm_end,pcm_start);
    OP_ASSERT(!ret);
    duration-=pre_skip;
    if(_pcm_offset<duration){
      _pcm_offset+=pre_skip;
      if(OP_UNLIKELY(pcm_start>OP_INT64_MAX-_pcm_offset)){
        /*Adding this amount to the granule position would overflow the positive
           half of its 64-bit range.
          Since signed overflow is undefined in C, do it in a way the compiler
           isn't allowed to screw up.*/
        _pcm_offset-=OP_INT64_MAX-pcm_start+1;
        pcm_start=OP_INT64_MIN;
      }
      pcm_start+=_pcm_offset;
      *_li=li;
      return pcm_start;
    }
    _pcm_offset-=duration;
  }
  return -1;
}

/*Rescale the number _x from the range [0,_from] to [0,_to].
  _from and _to must be positive.*/
opus_int64 op_rescale64(opus_int64 _x,opus_int64 _from,opus_int64 _to){
  opus_int64 frac;
  opus_int64 ret;
  int        i;
  if(_x>=_from)return _to;
  if(_x<=0)return 0;
  frac=0;
  for(i=0;i<63;i++){
    frac<<=1;
    OP_ASSERT(_x<=_from);
    if(_x>=_from>>1){
      _x-=_from-_x;
      frac|=1;
    }
    else _x<<=1;
  }
  ret=0;
  for(i=0;i<63;i++){
    if(frac&1)ret=(ret&_to&1)+(ret>>1)+(_to>>1);
    else ret>>=1;
    frac>>=1;
  }
  return ret;
}

/*Search within link _li for the page with the highest granule position
   preceding (or equal to) _target_gp.
  There is a danger here: missing pages or incorrect frame number information
   in the bitstream could make our task impossible.
  Account for that (it would be an error condition).*/
static int op_pcm_seek_page_impl(OggOpusFile *_of,
 ogg_int64_t _target_gp,int _li){
  OggOpusLink  *link;
  ogg_page      og;
  ogg_int64_t   pcm_pre_skip;
  ogg_int64_t   pcm_start;
  ogg_int64_t   pcm_end;
  ogg_int64_t   best_gp;
  ogg_int64_t   diff;
  ogg_uint32_t  serialno;
  opus_int32    pre_skip;
  opus_int32    cur_discard_count;
  opus_int64    begin;
  opus_int64    end;
  opus_int64    best;
  opus_int64    llret;
  int           ret;
  _of->bytes_tracked=0;
  _of->samples_tracked=0;
  op_decode_clear(_of);
  /*New search algorithm by HB (Nicholas Vinen).*/
  link=_of->links+_li;
  best_gp=pcm_start=link->pcm_start;
  pcm_end=link->pcm_end;
  serialno=link->serialno;
  best=begin=link->data_offset;
  /*We discard the first 80 ms of data after a seek, so seek back that much
     farther.
    If we can't, simply seek to the beginning of the link.*/
  if(OP_UNLIKELY(op_granpos_add(&_target_gp,_target_gp,-80*48)<0)){
    _target_gp=pcm_start;
  }
  /*Special case seeking to the start of the link.*/
  pre_skip=link->head.pre_skip;
  ret=op_granpos_add(&pcm_pre_skip,pcm_start,pre_skip);
  OP_ASSERT(!ret);
  end=op_granpos_cmp(_target_gp,pcm_pre_skip)<0?begin:link->end_offset;
  llret=OP_FALSE;
  while(begin<end){
    opus_int64 bisect;
    if(end-begin<OP_CHUNK_SIZE)bisect=begin;
    else{
      ogg_int64_t diff2;
      ret=op_granpos_diff(&diff,_target_gp,pcm_start);
      OP_ASSERT(!ret);
      ret=op_granpos_diff(&diff2,pcm_end,pcm_start);
      OP_ASSERT(!ret);
      /*Take a (pretty decent) guess.*/
      bisect=begin+op_rescale64(diff,diff2,end-begin)-OP_CHUNK_SIZE;
      if(bisect<begin+OP_CHUNK_SIZE)bisect=begin;
    }
    if(bisect!=_of->offset){
      ret=op_seek_helper(_of,bisect);
      if(OP_UNLIKELY(ret<0))return ret;
    }
    while(begin<end){
      llret=op_get_next_page(_of,&og,end-_of->offset);
      if(llret==OP_EREAD)return OP_EBADLINK;
      if(llret<0){
        /*Found it.*/
        if(bisect<=begin+1)end=begin;
        else{
          bisect=OP_MAX(bisect-OP_CHUNK_SIZE,begin+1);
          ret=op_seek_helper(_of,bisect);
          if(OP_UNLIKELY(ret<0))return ret;
        }
      }
      else{
        ogg_int64_t gp;
        if(serialno!=(ogg_uint32_t)ogg_page_serialno(&og))continue;
        gp=ogg_page_granulepos(&og);
        if(gp==-1)continue;
        if(op_granpos_cmp(gp,_target_gp)<0){
          /*Advance to the raw offset of the next page.*/
          begin=_of->offset;
          /*Don't let pcm_start get smaller!
            That could happen with an invalid timestamp.*/
          if(op_granpos_cmp(pcm_start,gp)<=0){
            /*Save the byte offset of the end of the page with this granule
               position.*/
            best=_of->offset;
            best_gp=pcm_start=gp;
          }
          if(OP_UNLIKELY(op_granpos_diff(&diff,_target_gp,pcm_start)<0)
           ||diff>48000){
            break;
          }
          /*NOT begin+1.*/
          bisect=begin;
        }
        else{
          /*Found it.*/
          if(bisect<=begin+1)end=begin;
          else{
            /*We're pretty close.
              We'd be stuck in an endless loop otherwise.*/
            if(end==_of->offset){
              end=llret;
              bisect=OP_MAX(bisect-OP_CHUNK_SIZE,begin+1);
              ret=op_seek_helper(_of,bisect);
              if(OP_UNLIKELY(ret<0))return ret;
            }
            else{
              end=bisect;
              /*Don't let pcm_end get larger!
                That could happen with an invalid timestamp.*/
              if(OP_LIKELY(op_granpos_cmp(pcm_end,gp)>0))pcm_end=gp;
              break;
            }
          }
        }
      }
    }
  }
  /*Found our page.
    Seek right after it and update prev_packet_gp and cur_discard_count.
    This is an easier case than op_raw_seek(), as we don't need to keep any
     packets from the page we found.*/
  /*Seek, if necessary.*/
  if(best!=_of->offset){
    ret=op_seek_helper(_of,best);
    if(OP_UNLIKELY(ret<0))return ret;
  }
  /*By default, discard 80 ms of data after a seek, unless we seek
     into the pre-skip region.*/
  cur_discard_count=80*48;
  ret=op_granpos_diff(&diff,best_gp,pcm_start);
  OP_ASSERT(!ret);
  OP_ASSERT(diff>=0);
  /*If we start at the beginning of the pre-skip region, or we're at least
     80 ms from the end of the pre-skip region, we discard to the end of the
     pre-skip region.
    Otherwise, we still use the 80 ms default, which will discard past the end
     of the pre-skip region.*/
  if(diff<=OP_MAX(0,pre_skip-80*48))cur_discard_count=pre_skip-(int)diff;
  _of->cur_link=_li;
  _of->ready_state=OP_STREAMSET;
  _of->prev_packet_gp=best_gp;
  _of->cur_discard_count=cur_discard_count;
  ogg_stream_reset_serialno(&_of->os,serialno);
  do ret=op_fetch_and_process_page(_of,1,0);
  /*Ignore holes.*/
  while(ret==OP_HOLE);
  if(OP_UNLIKELY(ret<=0))return OP_EBADLINK;
  /*Verify result.*/
  if(OP_UNLIKELY(op_granpos_cmp(_of->prev_packet_gp,_target_gp)>0)){
    return OP_EBADLINK;
  }
  return 0;
}

int op_pcm_seek_page(OggOpusFile *_of,ogg_int64_t _pcm_offset){
  ogg_int64_t target_gp;
  int         li;
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED))return OP_EINVAL;
  if(OP_UNLIKELY(!_of->seekable))return OP_ENOSEEK;
  if(OP_UNLIKELY(_pcm_offset<0))return OP_EINVAL;
  target_gp=op_get_granulepos(_of,_pcm_offset,&li);
  if(OP_UNLIKELY(target_gp==-1))return OP_EINVAL;
  return op_pcm_seek_page_impl(_of,target_gp,li);
}

int op_pcm_seek(OggOpusFile *_of,ogg_int64_t _pcm_offset){
  OggOpusLink *link;
  ogg_int64_t  pcm_start;
  ogg_int64_t  target_gp;
  ogg_int64_t  prev_packet_gp;
  ogg_int64_t  skip;
  ogg_int64_t  diff;
  int          op_count;
  int          op_pos;
  int          ret;
  int          li;
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED))return OP_EINVAL;
  if(OP_UNLIKELY(!_of->seekable))return OP_ENOSEEK;
  if(OP_UNLIKELY(_pcm_offset<0))return OP_EINVAL;
  target_gp=op_get_granulepos(_of,_pcm_offset,&li);
  if(OP_UNLIKELY(target_gp==-1))return OP_EINVAL;
  ret=op_pcm_seek_page_impl(_of,target_gp,li);
  /*Now skip samples until we actually get to our target.*/
  link=_of->links+li;
  pcm_start=link->pcm_start;
  ret=op_granpos_diff(&_pcm_offset,target_gp,pcm_start);
  OP_ASSERT(!ret);
  /*Figure out where we should skip to.*/
  if(_pcm_offset<=link->head.pre_skip)skip=0;
  else skip=OP_MAX(_pcm_offset-80*48,0);
  OP_ASSERT(_pcm_offset-skip>=0);
  OP_ASSERT(_pcm_offset-skip<0x7FFFFFFF-120*48);
  /*Skip packets until we find one with samples past our skip target.*/
  for(;;){
    op_count=_of->op_count;
    prev_packet_gp=_of->prev_packet_gp;
    for(op_pos=_of->op_pos;op_pos<op_count;op_pos++){
      ogg_int64_t cur_packet_gp;
      cur_packet_gp=_of->op[op_pos].granulepos;
      if(OP_LIKELY(!op_granpos_diff(&diff,cur_packet_gp,pcm_start))
       &&diff>skip){
        break;
      }
      prev_packet_gp=cur_packet_gp;
    }
    _of->prev_packet_gp=prev_packet_gp;
    _of->op_pos=op_pos;
    if(op_pos<op_count)break;
    /*We skipped all the packets on this page.
      Fetch another.*/
    do ret=op_fetch_and_process_page(_of,1,0);
    /*Ignore holes.*/
    while(ret==OP_HOLE);
    if(OP_UNLIKELY(ret<=0))return OP_EBADLINK;
  }
  ret=op_granpos_diff(&diff,prev_packet_gp,pcm_start);
  OP_ASSERT(!ret);
  /*We skipped too far.
    Either the timestamps were illegal or there was a hole in the data.*/
  if(diff>skip)return OP_EBADLINK;
  OP_ASSERT(_pcm_offset-diff<0x7FFFFFFF);
  /*TODO: If there are further holes/illegal timestamps, we still won't decode
     to the correct sample.
    However, at least op_pcm_tell() will report the correct value immediately
     after returning.*/
  _of->cur_discard_count=(opus_int32)(_pcm_offset-diff);
  return 0;
}

opus_int64 op_raw_tell(OggOpusFile *_of){
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED))return OP_EINVAL;
  return _of->offset;
}

/*Convert a granule position from a given link to a PCM offset relative to the
   start of the whole stream.
  For unseekable sources, this gets reset to 0 at the beginning of each link.*/
static ogg_int64_t op_get_pcm_offset(const OggOpusFile *_of,
 ogg_int64_t _gp,int _li){
  OggOpusLink *links;
  ogg_int64_t  pcm_offset;
  ogg_int64_t  delta;
  int          li;
  links=_of->links;
  pcm_offset=0;
  OP_ASSERT(_li<_of->nlinks);
  for(li=0;li<_li;li++){
    op_granpos_diff(&delta,links[li].pcm_end,links[li].pcm_start);
    delta-=links[li].head.pre_skip;
    pcm_offset+=delta;
  }
  OP_ASSERT(_li>=0);
  if(_of->seekable&&OP_UNLIKELY(op_granpos_cmp(_gp,links[_li].pcm_end)>0)){
    _gp=links[_li].pcm_end;
  }
  if(OP_LIKELY(op_granpos_cmp(_gp,links[_li].pcm_start)>0)){
    op_granpos_diff(&delta,_gp,links[_li].pcm_start);
    if(delta<links[_li].head.pre_skip)delta=0;
    else delta-=links[_li].head.pre_skip;
    pcm_offset+=delta;
  }
  return pcm_offset;
}

ogg_int64_t op_pcm_tell(OggOpusFile *_of){
  ogg_int64_t gp;
  int         nbuffered;
  int         ret;
  int         li;
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED))return OP_EINVAL;
  gp=_of->prev_packet_gp;
  if(gp==-1)return 0;
  nbuffered=OP_MAX(_of->od_buffer_size-_of->od_buffer_pos,0);
  ret=op_granpos_add(&gp,gp,-nbuffered);
  OP_ASSERT(!ret);
  li=_of->seekable?_of->cur_link:0;
  if(op_granpos_add(&gp,gp,_of->cur_discard_count)<0){
    gp=_of->links[li].pcm_end;
  }
  return op_get_pcm_offset(_of,gp,li);
}

/*Allocate the decoder scratch buffer.
  This is done lazily, since if the user provides large enough buffers, we'll
   never need it.*/
static int op_init_buffer(OggOpusFile *_of){
  int nchannels_max;
  if(_of->seekable){
    OggOpusLink *links;
    int          nlinks;
    int          li;
    links=_of->links;
    nlinks=_of->nlinks;
    nchannels_max=0;
    for(li=0;li<nlinks;li++){
      nchannels_max=OP_MAX(nchannels_max,links[li].head.channel_count);
    }
  }
  else nchannels_max=OP_NCHANNELS_MAX;
  _of->od_buffer=(op_sample *)_ogg_malloc(
   sizeof(*_of->od_buffer)*nchannels_max*120*48);
  if(_of->od_buffer==NULL)return OP_EFAULT;
  return 0;
}

/*Read more samples from the stream, using the same API as op_read() or
   op_read_float().*/
static int op_read_native(OggOpusFile *_of,
 op_sample *_pcm,int _buf_size,int *_li){
  if(OP_UNLIKELY(_of->ready_state<OP_OPENED))return OP_EINVAL;
  for(;;){
    int ret;
    if(OP_LIKELY(_of->ready_state>=OP_INITSET)){
      int nchannels;
      int od_buffer_pos;
      int nsamples;
      int op_pos;
      nchannels=_of->links[_of->seekable?_of->cur_link:0].head.channel_count;
      od_buffer_pos=_of->od_buffer_pos;
      nsamples=_of->od_buffer_size-od_buffer_pos;
      /*If we have buffered samples, return them.*/
      if(OP_UNLIKELY(nsamples>0)){
        if(OP_UNLIKELY(nsamples*nchannels>_buf_size)){
          nsamples=_buf_size/nchannels;
        }
        memcpy(_pcm,_of->od_buffer+nchannels*od_buffer_pos,
         sizeof(*_pcm)*nchannels*nsamples);
        od_buffer_pos+=nsamples;
        _of->od_buffer_pos=od_buffer_pos;
        if(_li!=NULL)*_li=_of->cur_link;
        return nsamples;
      }
      /*If we have buffered packets, decode one.*/
      op_pos=_of->op_pos;
      if(OP_LIKELY(op_pos<_of->op_count)){
        ogg_packet  *pop;
        ogg_int64_t  diff;
        opus_int32   cur_discard_count;
        int          duration;
        int          trimmed_duration;
        pop=_of->op+op_pos++;
        _of->op_pos=op_pos;
        cur_discard_count=_of->cur_discard_count;
        duration=op_get_packet_duration(pop->packet,pop->bytes);
        /*We don't buffer packets with an invalid TOC sequence.*/
        OP_ASSERT(duration>0);
        trimmed_duration=duration;
        /*Perform end-trimming.*/
        if(OP_UNLIKELY(pop->e_o_s)){
          if(OP_UNLIKELY(op_granpos_cmp(pop->granulepos,
           _of->prev_packet_gp)<=0)){
            trimmed_duration=0;
          }
          else if(OP_LIKELY(!op_granpos_diff(&diff,
           pop->granulepos,_of->prev_packet_gp))){
            trimmed_duration=(int)OP_MIN(diff,trimmed_duration);
          }
        }
        _of->prev_packet_gp=pop->granulepos;
        if(OP_UNLIKELY(duration*nchannels>_buf_size)){
          op_sample *buf;
          /*If the user's buffer is too small, decode into a scratch buffer.*/
          buf=_of->od_buffer;
          if(OP_UNLIKELY(buf==NULL)){
            ret=op_init_buffer(_of);
            if(OP_UNLIKELY(ret<0))return ret;
            buf=_of->od_buffer;
          }
#if defined(OP_FIXED_POINT)
          ret=opus_multistream_decode(_of->od,
           pop->packet,pop->bytes,buf,120*48,0);
#else
          ret=opus_multistream_decode_float(_of->od,
           pop->packet,pop->bytes,buf,120*48,0);
#endif
          if(OP_UNLIKELY(ret<0))return OP_EBADPACKET;
          OP_ASSERT(ret==duration);
          /*Perform pre-skip/pre-roll.*/
          od_buffer_pos=(int)OP_MIN(trimmed_duration,cur_discard_count);
          cur_discard_count-=od_buffer_pos;
          _of->cur_discard_count=cur_discard_count;
          _of->od_buffer_pos=od_buffer_pos;
          _of->od_buffer_size=trimmed_duration;
          /*Update bitrate tracking based on the actual samples we used from
             what was decoded.*/
          _of->bytes_tracked+=pop->bytes;
          _of->samples_tracked+=trimmed_duration-od_buffer_pos;
          /*Don't grab another page yet.*/
          if(OP_LIKELY(od_buffer_pos<trimmed_duration))continue;
        }
        else{
          /*Otherwise decode directly into the user's buffer.*/
#if defined(OP_FIXED_POINT)
          ret=opus_multistream_decode(_of->od,pop->packet,pop->bytes,
           _pcm,_buf_size/nchannels,0);
#else
          ret=opus_multistream_decode_float(_of->od,pop->packet,pop->bytes,
           _pcm,_buf_size/nchannels,0);
#endif
          if(OP_UNLIKELY(ret<0))return OP_EBADPACKET;
          OP_ASSERT(ret==duration);
          if(OP_LIKELY(trimmed_duration>0)){
            /*Perform pre-skip/pre-roll.*/
            od_buffer_pos=(int)OP_MIN(trimmed_duration,cur_discard_count);
            cur_discard_count-=od_buffer_pos;
            _of->cur_discard_count=cur_discard_count;
            if(OP_UNLIKELY(od_buffer_pos>0)
             &&OP_LIKELY(od_buffer_pos<trimmed_duration)){
              memmove(_pcm,_pcm+od_buffer_pos*nchannels,
               sizeof(*_pcm)*(trimmed_duration-od_buffer_pos)*nchannels);
            }
            trimmed_duration-=od_buffer_pos;
            /*Update bitrate tracking based on the actual samples we used from
               what was decoded.*/
            _of->bytes_tracked+=pop->bytes;
            _of->samples_tracked+=trimmed_duration;
            if(OP_LIKELY(trimmed_duration>0)){
              if(_li!=NULL)*_li=_of->cur_link;
              return trimmed_duration;
            }
          }
        }
      }
    }
    /*Suck in another page.*/
    ret=op_fetch_and_process_page(_of,1,1);
    if(OP_UNLIKELY(ret==OP_EOF)){
      if(_li!=NULL)*_li=_of->cur_link;
      return 0;
    }
    if(OP_UNLIKELY(ret<0))return ret;
  }
}

#if defined(OP_FIXED_POINT)

int op_read(OggOpusFile *_of,opus_int16 *_pcm,int _buf_size,int *_li){
  return op_read_native(_of,_pcm,_buf_size,_li);
}

# if !defined(OP_DISABLE_FLOAT_API)
int op_read_float(OggOpusFile *_of,float *_pcm,int _buf_size,int *_li){
  int ret;
  /*Ensure we have some decoded samples in our buffer.*/
  ret=op_read_native(_of,NULL,0,_li);
  /*Now convert them to float.*/
  if(OP_LIKELY(ret>=0)&&OP_LIKELY(_of->ready_state>=OP_INITSET)){
    int nchannels;
    int od_buffer_pos;
    nchannels=_of->links[_of->seekable?_of->cur_link:0].head.channel_count;
    od_buffer_pos=_of->od_buffer_pos;
    ret=_of->od_buffer_size-od_buffer_pos;
    if(OP_LIKELY(ret>0)){
      op_sample *buf;
      int        i;
      if(OP_UNLIKELY(ret*nchannels>_buf_size))ret=_buf_size/nchannels;
      buf=_of->od_buffer+nchannels*od_buffer_pos;
      _buf_size=ret*nchannels;
      for(i=0;i<_buf_size;i++)_pcm[i]=(1.0F/32768)*buf[i];
      od_buffer_pos+=ret;
      _of->od_buffer_pos=od_buffer_pos;
    }
  }
  return ret;
}
# endif

#else

# if defined(OP_HAVE_LRINTF)
#  include <math.h>
#  define op_float2int(_x) (lrintf(_x))
# else
#  define op_float2int(_x) ((int)((_x)+((_x)<0?-0.5F:0.5F)))
# endif

/*The dithering code here is adapted from opusdec, part of opus-tools.
  It was originally written by Greg Maxwell.*/

static opus_uint32 op_rand(opus_uint32 _seed){
  return _seed*96314165+907633515&0xFFFFFFFFU;
}

/*This implements 16-bit quantization with full triangular dither and IIR noise
   shaping.
  The noise shaping filters were designed by Sebastian Gesemann, and are based
   on the LAME ATH curves with flattening to limite their peak gain to 20 dB.
  Everyone else's noise shaping filters are mildly crazy.
  The 48 kHz version of this filter is just a warped version of the 44.1 kHz
   filter and probably could be improved by shifting the HF shelf up in
   frequency a little bit, since 48 kHz has a bit more room and being more
   conservative against bat-ears is probably more important than more noise
   suppression.
  This process can increase the peak level of the signal (in theory by the peak
   error of 1.5 +20 dB, though that is unobservably rare).
  To avoid clipping, the signal is attenuated by a couple thousands of a dB.
  Initially, the approach taken here was to only attenuate by the 99.9th
   percentile, making clipping rare but not impossible (like SoX), but the
   limited gain of the filter means that the worst case was only two
   thousandths of a dB more, so this just uses the worst case.
  The attenuation is probably also helpful to prevent clipping in the DAC
   reconstruction filters or downstream resampling, in any case.*/

#define OP_GAIN (32753.0F)

#define OP_PRNG_GAIN (1.0F/0xFFFFFFFF)

/*48 kHz noise shaping filter, sd=2.34.*/

static const float OP_FCOEF_B[4]={
  2.2374F,-0.7339F,-0.1251F,-0.6033F
};

static const float OP_FCOEF_A[4]={
  0.9030F,0.0116F,-0.5853F,-0.2571F
};

static void op_shaped_dither16(OggOpusFile *_of,opus_int16 *_dst,float *_src,
 int _nsamples,int _nchannels){
  opus_uint32 seed;
  int         mute;
  int         i;
  mute=_of->dither_mute;
  seed=_of->dither_seed;
  /*In order to avoid replacing digital silence with quiet dither noise, we
     mute if the output has been silent for a while.*/
  if(mute>64)memset(_of->dither_a,0,sizeof(*_of->dither_a)*4*_nchannels);
  for(i=0;i<_nsamples;i++){
    int silent;
    int ci;
    silent=1;
    for(ci=0;ci<_nchannels;ci++){
      float r;
      float s;
      float err;
      int   si;
      int   j;
      s=_src[_nchannels*i+ci];
      silent&=s==0;
      s*=OP_GAIN;
      err=0;
      for(j=0;j<4;j++){
        err+=OP_FCOEF_B[j]*_of->dither_b[ci*4+j]
         -OP_FCOEF_A[j]*_of->dither_a[ci*4+j];
      }
      for(j=3;j-->0;)_of->dither_a[ci*4+j+1]=_of->dither_a[ci*4+j];
      for(j=3;j-->0;)_of->dither_b[ci*4+j+1]=_of->dither_b[ci*4+j];
      _of->dither_a[ci*4]=err;
      s-=err;
      if(mute>16)r=0;
      else{
        seed=op_rand(seed);
        r=seed*OP_PRNG_GAIN;
        seed=op_rand(seed);
        r-=seed*OP_PRNG_GAIN;
      }
      /*Clamp in float out of paranoia that the input will be > 96 dBFS and
         wrap if the integer is clamped.*/
      si=op_float2int(OP_CLAMP(-32768,s+r,32767));
      _dst[_nchannels*i+ci]=(opus_int16)si;
      /*Including clipping in the noise shaping is generally disastrous: the
         futile effort to restore the clipped energy results in more clipping.
        However, small amounts---at the level which could normally be created
         by dither and rounding---are harmless and can even reduce clipping
         somewhat due to the clipping sometimes reducing the dither + rounding
         error.*/
      _of->dither_b[ci*4]=mute>16?0:OP_CLAMP(-1.5F,si-s,1.5F);
    }
    mute++;
    if(!silent)mute=0;
  }
  _of->dither_mute=OP_MIN(mute,65);
  _of->dither_seed=seed;
}

int op_read(OggOpusFile *_of,opus_int16 *_pcm,int _buf_size,int *_li){
  int ret;
  /*Ensure we have some decoded samples in our buffer.*/
  ret=op_read_native(_of,NULL,0,_li);
  /*Now convert them to shorts.*/
  if(OP_LIKELY(ret>=0)&&OP_LIKELY(_of->ready_state>=OP_INITSET)){
    int nchannels;
    int od_buffer_pos;
    nchannels=_of->links[_of->seekable?_of->cur_link:0].head.channel_count;
    od_buffer_pos=_of->od_buffer_pos;
    ret=_of->od_buffer_size-od_buffer_pos;
    if(OP_LIKELY(ret>0)){
      op_sample *buf;
      if(OP_UNLIKELY(ret*nchannels>_buf_size))ret=_buf_size/nchannels;
      buf=_of->od_buffer+nchannels*od_buffer_pos;
      op_shaped_dither16(_of,_pcm,buf,ret,nchannels);
      od_buffer_pos+=ret;
      _of->od_buffer_pos=od_buffer_pos;
    }
  }
  return ret;
}

int op_read_float(OggOpusFile *_of,float *_pcm,int _buf_size,int *_li){
  return op_read_native(_of,_pcm,_buf_size,_li);
}

#endif