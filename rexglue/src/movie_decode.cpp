// The movies' pictures decoded on the host.
//
// Every movie on the disc is WMV3 (VC-1 Main profile, 1280x720, I and P
// frames only) in ASF, played by the XDK's movie player. The player runs the
// title's own WMV decoder in a guest thread, and on the Xbox One that
// recompiled decoder makes about 20 of the clip's 30 frames a second on one
// whole core. FFmpeg's decoder makes several times that on the same core.
//
// The player drives the decoder through a table of entry points (the object
// at player+4, its glue struct at object+32):
//   +0  0x824C0590  set up / negotiate formats
//   +4  0x824C0248  flush
//   +8  0x824C02F8  decode one frame: *out = picture bytes, or 0
//   +12 0x824C0358  copy the picture out as planar 4:2:0 (Y, U, V)
//   +16 0x824C01F0  discontinuity
// and the decoder pulls its compressed bytes through a callback (0x824BFF30)
// that calls the player's fetch function (glue+0, with glue+4), which hands
// out a frame in pieces - pointer at glue+20, size at glue+12 - until it
// flags the frame's end. The player counts on those bytes being taken:
// skipping the decode without them puts it in an error state.
//
// So the decode entry is replaced: it pulls the frame through the same fetch
// function, exactly as the callback would, and FFmpeg decodes it; the copy-out
// entry writes FFmpeg's picture where the title's would have gone. Flush and
// discontinuity still reach the title's decoder (cheap) and flush FFmpeg's.
// XERENGE_GUEST_MOVIE_DECODER=1 keeps the title's own decoder.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/audio/wmv3_decoder.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>

#include "guest_memory.h"

extern "C" void __imp__sub_824C02F8(PPCContext& __restrict, uint8_t*);  // decode
extern "C" void __imp__sub_824C0358(PPCContext& __restrict, uint8_t*);  // copy out
extern "C" void __imp__sub_824C0248(PPCContext& __restrict, uint8_t*);  // flush
extern "C" void __imp__sub_824C01F0(PPCContext& __restrict, uint8_t*);  // discontinuity

namespace {

// The glue struct.
constexpr uint32_t kFetch = 0;          // the player's fetch function
constexpr uint32_t kFetchContext = 4;
constexpr uint32_t kChunkSize = 12;     // fetch's size out
constexpr uint32_t kRemaining = 16;     // bytes of the chunk not yet taken
constexpr uint32_t kChunkPointer = 20;  // fetch's pointer out, advanced as taken
constexpr uint32_t kWidth = 32;
constexpr uint32_t kHeight = 36;
constexpr uint32_t kDecoder = 68;       // the title's decoder state
constexpr uint32_t kTimestamp = 72;     // 64-bit, of the frame being decoded
constexpr uint32_t kPictureBytes = 108;
constexpr uint32_t kEndPending = 112;
// The title's decoder state: its pointer to the sequence header.
constexpr uint32_t kSequenceHeader = 18232;
constexpr uint32_t kSequenceHeaderSize = 4;  // WMV3's STRUCT_C

bool Enabled() {
  static const bool guest = [] {
    const char* v = std::getenv("XERENGE_GUEST_MOVIE_DECODER");
    return v && *v && *v != '0';
  }();
  return !guest;
}

struct Session {
  // The decoder is used from more than one thread: the movie thread decodes,
  // the player stops or seeks (Flush) and copies pictures out from others.
  // FFmpeg's decoder takes one caller at a time - a flush in the middle of a
  // decode freed the frame pool under it, and the heap corruption that left
  // hung the Xbox One a few seconds into the title screen's video.
  std::mutex decoder_mutex;
  rex::audio::Wmv3Decoder decoder;
  std::vector<uint8_t> frame;
  bool failed = false;
};

// Shared, so a new stream on the same glue struct (which drops the old session)
// cannot destroy a decoder another thread is still inside.
std::mutex sessions_mutex;
std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions;

std::shared_ptr<Session> SessionFor(uint32_t glue) {
  std::lock_guard lock(sessions_mutex);
  auto& s = sessions[glue];
  if (!s) {
    s = std::make_shared<Session>();
  }
  return s;
}

std::shared_ptr<Session> FindSession(uint32_t glue) {
  std::lock_guard lock(sessions_mutex);
  auto it = sessions.find(glue);
  return it == sessions.end() ? nullptr : it->second;
}

uint32_t Load(const uint8_t* base, uint32_t address) {
  return xerenge::LoadGuestU32(base, address);
}

void Store(uint8_t* base, uint32_t address, uint32_t value) {
  xerenge::StoreGuestU32(base, address, value);
}

// One call of the player's fetch function, as the callback makes it. The
// outputs it takes by pointer live in a scratch area below the guest stack.
// Returns false when the fetch failed (end of data, or stopping).
bool Fetch(PPCContext& ctx, uint8_t* base, uint32_t glue, bool& end_of_frame) {
  const uint64_t saved_r1 = ctx.r1.u64;
  const uint64_t saved_lr = ctx.lr;
  const uint32_t scratch = (ctx.r1.u32 - 128) & ~15u;
  std::memset(xerenge::GuestPointer(base, scratch), 0, 32);
  const uint32_t ts_valid = scratch, end = scratch + 4, unused_a = scratch + 8,
                 unused_b = scratch + 12, ts = scratch + 16;
  ctx.r1.u64 = scratch - 112;
  Store(base, uint32_t(ctx.r1.u64), uint32_t(saved_r1));  // back chain
  ctx.r3.u64 = Load(base, glue + kFetchContext);
  ctx.r4.u64 = glue + kChunkPointer;
  ctx.r5.u64 = glue + kChunkSize;
  ctx.r6.u64 = end;
  ctx.r7.u64 = ts;
  ctx.r8.u64 = ts_valid;
  ctx.r9.u64 = unused_a;
  ctx.r10.u64 = unused_b;
  rex::runtime::ResolveIndirectFunction(Load(base, glue + kFetch))(ctx, base);
  const int32_t status = int32_t(ctx.r3.u32);
  ctx.r1.u64 = saved_r1;
  ctx.lr = saved_lr;
  if (status < 0) {
    return false;
  }
  if (Load(base, ts_valid)) {
    std::memcpy(xerenge::GuestPointer(base, glue + kTimestamp), xerenge::GuestPointer(base, ts), 8);
  }
  end_of_frame = Load(base, end) != 0;
  Store(base, glue + kRemaining, Load(base, glue + kChunkSize));
  return true;
}

// Takes what is left of the current chunk into the frame.
void TakeRemaining(uint8_t* base, uint32_t glue, std::vector<uint8_t>& frame) {
  const uint32_t remaining = Load(base, glue + kRemaining);
  if (!remaining) {
    return;
  }
  const uint32_t pointer = Load(base, glue + kChunkPointer);
  const uint8_t* bytes = xerenge::GuestPointer(base, pointer);
  frame.insert(frame.end(), bytes, bytes + remaining);
  Store(base, glue + kChunkPointer, pointer + remaining);
  Store(base, glue + kRemaining, 0);
}

bool OpenDecoder(const uint8_t* base, uint32_t glue, Session& session) {
  const uint32_t header = Load(base, Load(base, glue + kDecoder) + kSequenceHeader);
  const int width = int(Load(base, glue + kWidth));
  const int height = int(Load(base, glue + kHeight));
  if (!header || width <= 0 || height <= 0 || (width | height) & 1) {
    return false;
  }
  if (!session.decoder.Open(xerenge::GuestPointer(base, header), kSequenceHeaderSize, width,
                            height)) {
    return false;
  }
  REXLOG_INFO("movie: decoding {}x{} on the host (FFmpeg WMV3)", width, height);
  return true;
}

}  // namespace

// Decode one frame. r3 the decoder object, r4 where the picture's size goes.
REX_HOOK_RAW(sub_824C02F8) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  const uint32_t glue = Load(base, object + 32);
  const std::shared_ptr<Session> held = SessionFor(glue);
  Session& session = *held;
  if (!Enabled() || session.failed) {
    __imp__sub_824C02F8(ctx, base);
    return;
  }
  bool opened;
  {
    std::lock_guard decoder_lock(session.decoder_mutex);
    opened = session.decoder.is_open() || OpenDecoder(base, glue, session);
  }
  if (!opened) {
    REXLOG_WARN("movie: the host decoder could not start; the title's decodes");
    session.failed = true;
    __imp__sub_824C02F8(ctx, base);
    return;
  }

  session.frame.clear();
  TakeRemaining(base, glue, session.frame);
  bool end_of_frame = false;
  bool fetched_any = false;
  while (!end_of_frame) {
    if (!Fetch(ctx, base, glue, end_of_frame)) {
      break;
    }
    fetched_any = true;
    TakeRemaining(base, glue, session.frame);
  }
  Store(base, glue + kEndPending, 0);
  if (!fetched_any && session.frame.empty()) {
    // Nothing more to read: the title's decoder meets the same end and
    // answers it the way the player expects.
    ctx.r3.u64 = object;
    ctx.r4.u64 = out;
    __imp__sub_824C02F8(ctx, base);
    return;
  }

  bool picture;
  {
    std::lock_guard decoder_lock(session.decoder_mutex);
    picture = session.decoder.Decode(session.frame.data(), session.frame.size());
  }
  Store(base, out, picture ? Load(base, glue + kPictureBytes) : 0);
  ctx.r3.u64 = 0;
}

// Copy the picture out. r3 the object, r4 the buffer, r5 its size, r6 the
// bytes written, r7 a flag cleared, r8 the timestamp (64-bit), r9 optional
// flag set.
REX_HOOK_RAW(sub_824C0358) {
  const uint32_t glue = Load(base, ctx.r3.u32 + 32);
  const std::shared_ptr<Session> session = FindSession(glue);
  std::unique_lock<std::mutex> decoder_lock;
  if (session) {
    decoder_lock = std::unique_lock(session->decoder_mutex);
  }
  if (!Enabled() || !session || session->failed || !session->decoder.is_open()) {
    if (decoder_lock.owns_lock()) {
      decoder_lock.unlock();
    }
    __imp__sub_824C0358(ctx, base);
    return;
  }
  const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32, written = ctx.r6.u32,
                 flag = ctx.r7.u32, timestamp = ctx.r8.u32, done = ctx.r9.u32;
  const uint32_t picture_bytes = Load(base, glue + kPictureBytes);
  const int width = int(Load(base, glue + kWidth));
  const int height = int(Load(base, glue + kHeight));
  if (picture_bytes > size || (height & 1)) {
    ctx.r3.u64 = 0x80500006u;
    return;
  }
  uint8_t* y = xerenge::GuestPointer(base, buffer);
  uint8_t* u = y + size_t(width) * height;
  uint8_t* v = u + size_t(width / 2) * (height / 2);
  const bool copied = session->decoder.CopyPlanes(y, u, v, width, height);
  Store(base, written, copied ? picture_bytes : 0);
  Store(base, flag, 0);
  std::memcpy(xerenge::GuestPointer(base, timestamp), xerenge::GuestPointer(base, glue + kTimestamp),
              8);
  if (done) {
    Store(base, done, 1);
  }
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(sub_824C0248) {
  if (const std::shared_ptr<Session> session = FindSession(Load(base, ctx.r3.u32 + 32))) {
    std::lock_guard decoder_lock(session->decoder_mutex);
    session->decoder.Flush();
  }
  __imp__sub_824C0248(ctx, base);
}

REX_HOOK_RAW(sub_824C01F0) {
  if (const std::shared_ptr<Session> session = FindSession(Load(base, ctx.r3.u32 + 32))) {
    std::lock_guard decoder_lock(session->decoder_mutex);
    session->decoder.Flush();
  }
  __imp__sub_824C01F0(ctx, base);
}

// Set-up: a new stream on this glue struct (the player reuses the memory).
extern "C" void __imp__sub_824C0590(PPCContext& __restrict, uint8_t*);
REX_HOOK_RAW(sub_824C0590) {
  const uint32_t glue = Load(base, ctx.r3.u32 + 32);
  {
    std::lock_guard lock(sessions_mutex);
    sessions.erase(glue);
  }
  __imp__sub_824C0590(ctx, base);
}
