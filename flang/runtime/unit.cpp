//===-- runtime/unit.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "unit.h"
#include "io-error.h"
#include "lock.h"
#include "tools.h"
#include "unit-map.h"
#include <cstdio>
#include <limits>
#include <utility>

namespace Fortran::runtime::io {

// The per-unit data structures are created on demand so that Fortran I/O
// should work without a Fortran main program.
static Lock unitMapLock;
static Lock createOpenLock;
static UnitMap *unitMap{nullptr};
static ExternalFileUnit *defaultInput{nullptr}; // unit 5
static ExternalFileUnit *defaultOutput{nullptr}; // unit 6
static ExternalFileUnit *errorOutput{nullptr}; // unit 0 extension

void FlushOutputOnCrash(const Terminator &terminator) {
  if (!defaultOutput && !errorOutput) {
    return;
  }
  IoErrorHandler handler{terminator};
  handler.HasIoStat(); // prevent nested crash if flush has error
  CriticalSection critical{unitMapLock};
  if (defaultOutput) {
    defaultOutput->FlushOutput(handler);
  }
  if (errorOutput) {
    errorOutput->FlushOutput(handler);
  }
}

ExternalFileUnit *ExternalFileUnit::LookUp(int unit) {
  return GetUnitMap().LookUp(unit);
}

ExternalFileUnit *ExternalFileUnit::LookUpOrCreate(
    int unit, const Terminator &terminator, bool &wasExtant) {
  return GetUnitMap().LookUpOrCreate(unit, terminator, wasExtant);
}

ExternalFileUnit *ExternalFileUnit::LookUpOrCreateAnonymous(int unit,
    Direction dir, std::optional<bool> isUnformatted,
    const Terminator &terminator) {
  // Make sure that the returned anonymous unit has been opened
  // not just created in the unitMap.
  CriticalSection critical{createOpenLock};
  bool exists{false};
  ExternalFileUnit *result{
      GetUnitMap().LookUpOrCreate(unit, terminator, exists)};
  if (result && !exists) {
    IoErrorHandler handler{terminator};
    result->OpenAnonymousUnit(
        dir == Direction::Input ? OpenStatus::Unknown : OpenStatus::Replace,
        Action::ReadWrite, Position::Rewind, Convert::Unknown, handler);
    result->isUnformatted = isUnformatted;
  }
  return result;
}

ExternalFileUnit *ExternalFileUnit::LookUp(
    const char *path, std::size_t pathLen) {
  return GetUnitMap().LookUp(path, pathLen);
}

ExternalFileUnit &ExternalFileUnit::CreateNew(
    int unit, const Terminator &terminator) {
  bool wasExtant{false};
  ExternalFileUnit *result{
      GetUnitMap().LookUpOrCreate(unit, terminator, wasExtant)};
  RUNTIME_CHECK(terminator, result && !wasExtant);
  return *result;
}

ExternalFileUnit *ExternalFileUnit::LookUpForClose(int unit) {
  return GetUnitMap().LookUpForClose(unit);
}

ExternalFileUnit &ExternalFileUnit::NewUnit(
    const Terminator &terminator, bool forChildIo) {
  ExternalFileUnit &unit{GetUnitMap().NewUnit(terminator)};
  unit.createdForInternalChildIo_ = forChildIo;
  return unit;
}

bool ExternalFileUnit::OpenUnit(std::optional<OpenStatus> status,
    std::optional<Action> action, Position position, OwningPtr<char> &&newPath,
    std::size_t newPathLength, Convert convert, IoErrorHandler &handler) {
  if (convert == Convert::Unknown) {
    convert = executionEnvironment.conversion;
  }
  swapEndianness_ = convert == Convert::Swap ||
      (convert == Convert::LittleEndian && !isHostLittleEndian) ||
      (convert == Convert::BigEndian && isHostLittleEndian);
  bool impliedClose{false};
  if (IsConnected()) {
    bool isSamePath{newPath.get() && path() && pathLength() == newPathLength &&
        std::memcmp(path(), newPath.get(), newPathLength) == 0};
    if (status && *status != OpenStatus::Old && isSamePath) {
      handler.SignalError("OPEN statement for connected unit may not have "
                          "explicit STATUS= other than 'OLD'");
      return impliedClose;
    }
    if (!newPath.get() || isSamePath) {
      // OPEN of existing unit, STATUS='OLD' or unspecified, not new FILE=
      newPath.reset();
      return impliedClose;
    }
    // Otherwise, OPEN on open unit with new FILE= implies CLOSE
    DoImpliedEndfile(handler);
    FlushOutput(handler);
    TruncateFrame(0, handler);
    Close(CloseStatus::Keep, handler);
    impliedClose = true;
  }
  if (newPath.get() && newPathLength > 0) {
    if (const auto *already{
            GetUnitMap().LookUp(newPath.get(), newPathLength)}) {
      handler.SignalError(IostatOpenAlreadyConnected,
          "OPEN(UNIT=%d,FILE='%.*s'): file is already connected to unit %d",
          unitNumber_, static_cast<int>(newPathLength), newPath.get(),
          already->unitNumber_);
      return impliedClose;
    }
  }
  set_path(std::move(newPath), newPathLength);
  Open(status.value_or(OpenStatus::Unknown), action, position, handler);
  auto totalBytes{knownSize()};
  if (access == Access::Direct) {
    if (!openRecl) {
      handler.SignalError(IostatOpenBadRecl,
          "OPEN(UNIT=%d,ACCESS='DIRECT'): record length is not known",
          unitNumber());
    } else if (*openRecl <= 0) {
      handler.SignalError(IostatOpenBadRecl,
          "OPEN(UNIT=%d,ACCESS='DIRECT',RECL=%jd): record length is invalid",
          unitNumber(), static_cast<std::intmax_t>(*openRecl));
    } else if (totalBytes && (*totalBytes % *openRecl != 0)) {
      handler.SignalError(IostatOpenBadRecl,
          "OPEN(UNIT=%d,ACCESS='DIRECT',RECL=%jd): record length is not an "
          "even divisor of the file size %jd",
          unitNumber(), static_cast<std::intmax_t>(*openRecl),
          static_cast<std::intmax_t>(*totalBytes));
    }
    recordLength = openRecl;
  }
  endfileRecordNumber.reset();
  currentRecordNumber = 1;
  if (totalBytes && access == Access::Direct && openRecl.value_or(0) > 0) {
    endfileRecordNumber = 1 + (*totalBytes / *openRecl);
  }
  if (position == Position::Append) {
    if (totalBytes) {
      frameOffsetInFile_ = *totalBytes;
    }
    if (access != Access::Stream) {
      if (!endfileRecordNumber) {
        // Fake it so that we can backspace relative from the end
        endfileRecordNumber = std::numeric_limits<std::int64_t>::max() - 2;
      }
      currentRecordNumber = *endfileRecordNumber;
    }
  }
  return impliedClose;
}

void ExternalFileUnit::OpenAnonymousUnit(std::optional<OpenStatus> status,
    std::optional<Action> action, Position position, Convert convert,
    IoErrorHandler &handler) {
  // I/O to an unconnected unit reads/creates a local file, e.g. fort.7
  std::size_t pathMaxLen{32};
  auto path{SizedNew<char>{handler}(pathMaxLen)};
  std::snprintf(path.get(), pathMaxLen, "fort.%d", unitNumber_);
  OpenUnit(status, action, position, std::move(path), std::strlen(path.get()),
      convert, handler);
}

void ExternalFileUnit::CloseUnit(CloseStatus status, IoErrorHandler &handler) {
  DoImpliedEndfile(handler);
  FlushOutput(handler);
  Close(status, handler);
}

void ExternalFileUnit::DestroyClosed() {
  GetUnitMap().DestroyClosed(*this); // destroys *this
}

Iostat ExternalFileUnit::SetDirection(Direction direction) {
  if (direction == Direction::Input) {
    if (mayRead()) {
      direction_ = Direction::Input;
      return IostatOk;
    } else {
      return IostatReadFromWriteOnly;
    }
  } else {
    if (mayWrite()) {
      direction_ = Direction::Output;
      return IostatOk;
    } else {
      return IostatWriteToReadOnly;
    }
  }
}

UnitMap &ExternalFileUnit::CreateUnitMap() {
  Terminator terminator{__FILE__, __LINE__};
  IoErrorHandler handler{terminator};
  UnitMap &newUnitMap{*New<UnitMap>{terminator}().release()};

  bool wasExtant{false};
  ExternalFileUnit &out{*newUnitMap.LookUpOrCreate(6, terminator, wasExtant)};
  RUNTIME_CHECK(terminator, !wasExtant);
  out.Predefine(1);
  handler.SignalError(out.SetDirection(Direction::Output));
  out.isUnformatted = false;
  defaultOutput = &out;

  ExternalFileUnit &in{*newUnitMap.LookUpOrCreate(5, terminator, wasExtant)};
  RUNTIME_CHECK(terminator, !wasExtant);
  in.Predefine(0);
  handler.SignalError(in.SetDirection(Direction::Input));
  in.isUnformatted = false;
  defaultInput = &in;

  ExternalFileUnit &error{*newUnitMap.LookUpOrCreate(0, terminator, wasExtant)};
  RUNTIME_CHECK(terminator, !wasExtant);
  error.Predefine(2);
  handler.SignalError(error.SetDirection(Direction::Output));
  error.isUnformatted = false;
  errorOutput = &error;

  return newUnitMap;
}

// A back-up atexit() handler for programs that don't terminate with a main
// program END or a STOP statement or other Fortran-initiated program shutdown,
// such as programs with a C main() that terminate normally.  It flushes all
// external I/O units.  It is registered once the first time that any external
// I/O is attempted.
static void CloseAllExternalUnits() {
  IoErrorHandler handler{"Fortran program termination"};
  ExternalFileUnit::CloseAll(handler);
}

UnitMap &ExternalFileUnit::GetUnitMap() {
  if (unitMap) {
    return *unitMap;
  }
  {
    CriticalSection critical{unitMapLock};
    if (unitMap) {
      return *unitMap;
    }
    unitMap = &CreateUnitMap();
  }
  std::atexit(CloseAllExternalUnits);
  return *unitMap;
}

void ExternalFileUnit::CloseAll(IoErrorHandler &handler) {
  CriticalSection critical{unitMapLock};
  if (unitMap) {
    unitMap->CloseAll(handler);
    FreeMemoryAndNullify(unitMap);
  }
  defaultOutput = nullptr;
  defaultInput = nullptr;
  errorOutput = nullptr;
}

void ExternalFileUnit::FlushAll(IoErrorHandler &handler) {
  CriticalSection critical{unitMapLock};
  if (unitMap) {
    unitMap->FlushAll(handler);
  }
}

static inline void SwapEndianness(
    char *data, std::size_t bytes, std::size_t elementBytes) {
  if (elementBytes > 1) {
    auto half{elementBytes >> 1};
    for (std::size_t j{0}; j + elementBytes <= bytes; j += elementBytes) {
      for (std::size_t k{0}; k < half; ++k) {
        std::swap(data[j + k], data[j + elementBytes - 1 - k]);
      }
    }
  }
}

bool ExternalFileUnit::Emit(const char *data, std::size_t bytes,
    std::size_t elementBytes, IoErrorHandler &handler) {
  auto furthestAfter{std::max(furthestPositionInRecord,
      positionInRecord + static_cast<std::int64_t>(bytes))};
  if (openRecl) {
    // Check for fixed-length record overrun, but allow for
    // sequential record termination.
    int extra{0};
    int header{0};
    if (access == Access::Sequential) {
      if (isUnformatted.value_or(false)) {
        // record header + footer
        header = static_cast<int>(sizeof(std::uint32_t));
        extra = 2 * header;
      } else {
#ifdef _WIN32
        if (!isWindowsTextFile()) {
          ++extra; // carriage return (CR)
        }
#endif
        ++extra; // newline (LF)
      }
    }
    if (furthestAfter > extra + *openRecl) {
      handler.SignalError(IostatRecordWriteOverrun,
          "Attempt to write %zd bytes to position %jd in a fixed-size record "
          "of %jd bytes",
          bytes, static_cast<std::intmax_t>(positionInRecord - header),
          static_cast<std::intmax_t>(*openRecl));
      return false;
    }
  }
  if (recordLength) {
    // It is possible for recordLength to have a value now for a
    // variable-length output record if the previous operation
    // was a BACKSPACE or non advancing input statement.
    recordLength.reset();
    beganReadingRecord_ = false;
  }
  if (IsAfterEndfile()) {
    handler.SignalError(IostatWriteAfterEndfile);
    return false;
  }
  CheckDirectAccess(handler);
  WriteFrame(frameOffsetInFile_, recordOffsetInFrame_ + furthestAfter, handler);
  if (positionInRecord > furthestPositionInRecord) {
    std::memset(Frame() + recordOffsetInFrame_ + furthestPositionInRecord, ' ',
        positionInRecord - furthestPositionInRecord);
  }
  char *to{Frame() + recordOffsetInFrame_ + positionInRecord};
  std::memcpy(to, data, bytes);
  if (swapEndianness_) {
    SwapEndianness(to, bytes, elementBytes);
  }
  positionInRecord += bytes;
  furthestPositionInRecord = furthestAfter;
  return true;
}

bool ExternalFileUnit::Receive(char *data, std::size_t bytes,
    std::size_t elementBytes, IoErrorHandler &handler) {
  RUNTIME_CHECK(handler, direction_ == Direction::Input);
  auto furthestAfter{std::max(furthestPositionInRecord,
      positionInRecord + static_cast<std::int64_t>(bytes))};
  if (furthestAfter > recordLength.value_or(furthestAfter)) {
    handler.SignalError(IostatRecordReadOverrun,
        "Attempt to read %zd bytes at position %jd in a record of %jd bytes",
        bytes, static_cast<std::intmax_t>(positionInRecord),
        static_cast<std::intmax_t>(*recordLength));
    return false;
  }
  auto need{recordOffsetInFrame_ + furthestAfter};
  auto got{ReadFrame(frameOffsetInFile_, need, handler)};
  if (got >= need) {
    std::memcpy(data, Frame() + recordOffsetInFrame_ + positionInRecord, bytes);
    if (swapEndianness_) {
      SwapEndianness(data, bytes, elementBytes);
    }
    positionInRecord += bytes;
    furthestPositionInRecord = furthestAfter;
    return true;
  } else {
    HitEndOnRead(handler);
    return false;
  }
}

std::size_t ExternalFileUnit::GetNextInputBytes(
    const char *&p, IoErrorHandler &handler) {
  RUNTIME_CHECK(handler, direction_ == Direction::Input);
  std::size_t length{1};
  if (auto recl{EffectiveRecordLength()}) {
    if (positionInRecord < *recl) {
      length = *recl - positionInRecord;
    } else {
      p = nullptr;
      return 0;
    }
  }
  p = FrameNextInput(handler, length);
  return p ? length : 0;
}

const char *ExternalFileUnit::FrameNextInput(
    IoErrorHandler &handler, std::size_t bytes) {
  RUNTIME_CHECK(handler, isUnformatted.has_value() && !*isUnformatted);
  if (static_cast<std::int64_t>(positionInRecord + bytes) <=
      recordLength.value_or(positionInRecord + bytes)) {
    auto at{recordOffsetInFrame_ + positionInRecord};
    auto need{static_cast<std::size_t>(at + bytes)};
    auto got{ReadFrame(frameOffsetInFile_, need, handler)};
    SetVariableFormattedRecordLength();
    if (got >= need) {
      return Frame() + at;
    }
    HitEndOnRead(handler);
  }
  return nullptr;
}

bool ExternalFileUnit::SetVariableFormattedRecordLength() {
  if (recordLength || access == Access::Direct) {
    return true;
  } else if (FrameLength() > recordOffsetInFrame_) {
    const char *record{Frame() + recordOffsetInFrame_};
    std::size_t bytes{FrameLength() - recordOffsetInFrame_};
    if (const char *nl{FindCharacter(record, '\n', bytes)}) {
      recordLength = nl - record;
      if (*recordLength > 0 && record[*recordLength - 1] == '\r') {
        --*recordLength;
      }
      return true;
    }
  }
  return false;
}

bool ExternalFileUnit::BeginReadingRecord(IoErrorHandler &handler) {
  RUNTIME_CHECK(handler, direction_ == Direction::Input);
  if (!beganReadingRecord_) {
    beganReadingRecord_ = true;
    if (access == Access::Direct) {
      CheckDirectAccess(handler);
      auto need{static_cast<std::size_t>(recordOffsetInFrame_ + *openRecl)};
      auto got{ReadFrame(frameOffsetInFile_, need, handler)};
      if (got >= need) {
        recordLength = openRecl;
      } else {
        recordLength.reset();
        HitEndOnRead(handler);
      }
    } else {
      recordLength.reset();
      if (IsAtEOF()) {
        handler.SignalEnd();
      } else {
        RUNTIME_CHECK(handler, isUnformatted.has_value());
        if (*isUnformatted) {
          if (access == Access::Sequential) {
            BeginSequentialVariableUnformattedInputRecord(handler);
          }
        } else { // formatted sequential or stream
          BeginVariableFormattedInputRecord(handler);
        }
      }
    }
  }
  RUNTIME_CHECK(handler,
      recordLength.has_value() || !IsRecordFile() || handler.InError());
  return !handler.InError();
}

void ExternalFileUnit::FinishReadingRecord(IoErrorHandler &handler) {
  RUNTIME_CHECK(handler, direction_ == Direction::Input && beganReadingRecord_);
  beganReadingRecord_ = false;
  if (handler.GetIoStat() == IostatEnd ||
      (IsRecordFile() && !recordLength.has_value())) {
    // Avoid bogus crashes in END/ERR circumstances; but
    // still increment the current record number so that
    // an attempted read of an endfile record, followed by
    // a BACKSPACE, will still be at EOF.
    ++currentRecordNumber;
  } else if (IsRecordFile()) {
    recordOffsetInFrame_ += *recordLength;
    if (access != Access::Direct) {
      RUNTIME_CHECK(handler, isUnformatted.has_value());
      recordLength.reset();
      if (isUnformatted.value_or(false)) {
        // Retain footer in frame for more efficient BACKSPACE
        frameOffsetInFile_ += recordOffsetInFrame_;
        recordOffsetInFrame_ = sizeof(std::uint32_t);
      } else { // formatted
        if (FrameLength() > recordOffsetInFrame_ &&
            Frame()[recordOffsetInFrame_] == '\r') {
          ++recordOffsetInFrame_;
        }
        if (FrameLength() > recordOffsetInFrame_ &&
            Frame()[recordOffsetInFrame_] == '\n') {
          ++recordOffsetInFrame_;
        }
        if (!pinnedFrame || mayPosition()) {
          frameOffsetInFile_ += recordOffsetInFrame_;
          recordOffsetInFrame_ = 0;
        }
      }
    }
    ++currentRecordNumber;
  } else { // unformatted stream
    furthestPositionInRecord =
        std::max(furthestPositionInRecord, positionInRecord);
    frameOffsetInFile_ += recordOffsetInFrame_ + furthestPositionInRecord;
  }
  BeginRecord();
}

bool ExternalFileUnit::AdvanceRecord(IoErrorHandler &handler) {
  if (direction_ == Direction::Input) {
    FinishReadingRecord(handler);
    return BeginReadingRecord(handler);
  } else { // Direction::Output
    bool ok{true};
    RUNTIME_CHECK(handler, isUnformatted.has_value());
    positionInRecord = furthestPositionInRecord;
    if (access == Access::Direct) {
      if (furthestPositionInRecord <
          openRecl.value_or(furthestPositionInRecord)) {
        // Pad remainder of fixed length record
        WriteFrame(
            frameOffsetInFile_, recordOffsetInFrame_ + *openRecl, handler);
        std::memset(Frame() + recordOffsetInFrame_ + furthestPositionInRecord,
            isUnformatted.value_or(false) ? 0 : ' ',
            *openRecl - furthestPositionInRecord);
        furthestPositionInRecord = *openRecl;
      }
    } else if (*isUnformatted) {
      if (access == Access::Sequential) {
        // Append the length of a sequential unformatted variable-length record
        // as its footer, then overwrite the reserved first four bytes of the
        // record with its length as its header.  These four bytes were skipped
        // over in BeginUnformattedIO<Output>().
        // TODO: Break very large records up into subrecords with negative
        // headers &/or footers
        std::uint32_t length;
        length = furthestPositionInRecord - sizeof length;
        ok = ok &&
            Emit(reinterpret_cast<const char *>(&length), sizeof length,
                sizeof length, handler);
        positionInRecord = 0;
        ok = ok &&
            Emit(reinterpret_cast<const char *>(&length), sizeof length,
                sizeof length, handler);
      } else {
        // Unformatted stream: nothing to do
      }
    } else if (handler.GetIoStat() != IostatOk &&
        furthestPositionInRecord == 0) {
      // Error in formatted variable length record, and no output yet; do
      // nothing, like most other Fortran compilers do.
      return true;
    } else {
      // Terminate formatted variable length record
      const char *lineEnding{"\n"};
      std::size_t lineEndingBytes{1};
#ifdef _WIN32
      if (!isWindowsTextFile()) {
        lineEnding = "\r\n";
        lineEndingBytes = 2;
      }
#endif
      ok = ok && Emit(lineEnding, lineEndingBytes, 1, handler);
    }
    leftTabLimit.reset();
    if (IsAfterEndfile()) {
      return false;
    }
    CommitWrites();
    ++currentRecordNumber;
    if (access != Access::Direct) {
      impliedEndfile_ = IsRecordFile();
      if (IsAtEOF()) {
        endfileRecordNumber.reset();
      }
    }
    return ok;
  }
}

void ExternalFileUnit::BackspaceRecord(IoErrorHandler &handler) {
  if (access == Access::Direct || !IsRecordFile()) {
    handler.SignalError(IostatBackspaceNonSequential,
        "BACKSPACE(UNIT=%d) on direct-access file or unformatted stream",
        unitNumber());
  } else {
    if (IsAfterEndfile()) {
      // BACKSPACE after explicit ENDFILE
      currentRecordNumber = *endfileRecordNumber;
    } else if (leftTabLimit) {
      // BACKSPACE after non-advancing I/O
      leftTabLimit.reset();
    } else {
      DoImpliedEndfile(handler);
      if (frameOffsetInFile_ + recordOffsetInFrame_ > 0) {
        --currentRecordNumber;
        if (openRecl && access == Access::Direct) {
          BackspaceFixedRecord(handler);
        } else {
          RUNTIME_CHECK(handler, isUnformatted.has_value());
          if (isUnformatted.value_or(false)) {
            BackspaceVariableUnformattedRecord(handler);
          } else {
            BackspaceVariableFormattedRecord(handler);
          }
        }
      }
    }
    BeginRecord();
  }
}

void ExternalFileUnit::FlushOutput(IoErrorHandler &handler) {
  if (!mayPosition()) {
    auto frameAt{FrameAt()};
    if (frameOffsetInFile_ >= frameAt &&
        frameOffsetInFile_ <
            static_cast<std::int64_t>(frameAt + FrameLength())) {
      // A Flush() that's about to happen to a non-positionable file
      // needs to advance frameOffsetInFile_ to prevent attempts at
      // impossible seeks
      CommitWrites();
      leftTabLimit.reset();
    }
  }
  Flush(handler);
}

void ExternalFileUnit::FlushIfTerminal(IoErrorHandler &handler) {
  if (isTerminal()) {
    FlushOutput(handler);
  }
}

void ExternalFileUnit::Endfile(IoErrorHandler &handler) {
  if (access == Access::Direct) {
    handler.SignalError(IostatEndfileDirect,
        "ENDFILE(UNIT=%d) on direct-access file", unitNumber());
  } else if (!mayWrite()) {
    handler.SignalError(IostatEndfileUnwritable,
        "ENDFILE(UNIT=%d) on read-only file", unitNumber());
  } else if (IsAfterEndfile()) {
    // ENDFILE after ENDFILE
  } else {
    DoEndfile(handler);
    if (IsRecordFile() && access != Access::Direct) {
      // Explicit ENDFILE leaves position *after* the endfile record
      RUNTIME_CHECK(handler, endfileRecordNumber.has_value());
      currentRecordNumber = *endfileRecordNumber + 1;
    }
  }
}

void ExternalFileUnit::Rewind(IoErrorHandler &handler) {
  if (access == Access::Direct) {
    handler.SignalError(IostatRewindNonSequential,
        "REWIND(UNIT=%d) on non-sequential file", unitNumber());
  } else {
    SetPosition(0, handler);
    currentRecordNumber = 1;
    leftTabLimit.reset();
  }
}

void ExternalFileUnit::SetPosition(std::int64_t pos, IoErrorHandler &handler) {
  DoImpliedEndfile(handler);
  frameOffsetInFile_ = pos;
  recordOffsetInFrame_ = 0;
  if (access == Access::Direct) {
    directAccessRecWasSet_ = true;
  }
  BeginRecord();
}

bool ExternalFileUnit::SetStreamPos(
    std::int64_t oneBasedPos, IoErrorHandler &handler) {
  if (access != Access::Stream) {
    handler.SignalError("POS= may not appear unless ACCESS='STREAM'");
    return false;
  }
  if (oneBasedPos < 1) { // POS=1 is beginning of file (12.6.2.11)
    handler.SignalError(
        "POS=%zd is invalid", static_cast<std::intmax_t>(oneBasedPos));
    return false;
  }
  SetPosition(oneBasedPos - 1, handler);
  // We no longer know which record we're in.  Set currentRecordNumber to
  // a large value from whence we can both advance and backspace.
  currentRecordNumber = std::numeric_limits<std::int64_t>::max() / 2;
  endfileRecordNumber.reset();
  return true;
}

bool ExternalFileUnit::SetDirectRec(
    std::int64_t oneBasedRec, IoErrorHandler &handler) {
  if (access != Access::Direct) {
    handler.SignalError("REC= may not appear unless ACCESS='DIRECT'");
    return false;
  }
  if (!openRecl) {
    handler.SignalError("RECL= was not specified");
    return false;
  }
  if (oneBasedRec < 1) {
    handler.SignalError(
        "REC=%zd is invalid", static_cast<std::intmax_t>(oneBasedRec));
    return false;
  }
  currentRecordNumber = oneBasedRec;
  SetPosition((oneBasedRec - 1) * *openRecl, handler);
  return true;
}

void ExternalFileUnit::EndIoStatement() {
  io_.reset();
  u_.emplace<std::monostate>();
  lock_.Drop();
}

void ExternalFileUnit::BeginSequentialVariableUnformattedInputRecord(
    IoErrorHandler &handler) {
  std::int32_t header{0}, footer{0};
  std::size_t need{recordOffsetInFrame_ + sizeof header};
  std::size_t got{ReadFrame(frameOffsetInFile_, need, handler)};
  // Try to emit informative errors to help debug corrupted files.
  const char *error{nullptr};
  if (got < need) {
    if (got == recordOffsetInFrame_) {
      HitEndOnRead(handler);
    } else {
      error = "Unformatted variable-length sequential file input failed at "
              "record #%jd (file offset %jd): truncated record header";
    }
  } else {
    header = ReadHeaderOrFooter(recordOffsetInFrame_);
    recordLength = sizeof header + header; // does not include footer
    need = recordOffsetInFrame_ + *recordLength + sizeof footer;
    got = ReadFrame(frameOffsetInFile_, need, handler);
    if (got < need) {
      error = "Unformatted variable-length sequential file input failed at "
              "record #%jd (file offset %jd): hit EOF reading record with "
              "length %jd bytes";
    } else {
      footer = ReadHeaderOrFooter(recordOffsetInFrame_ + *recordLength);
      if (footer != header) {
        error = "Unformatted variable-length sequential file input failed at "
                "record #%jd (file offset %jd): record header has length %jd "
                "that does not match record footer (%jd)";
      }
    }
  }
  if (error) {
    handler.SignalError(error, static_cast<std::intmax_t>(currentRecordNumber),
        static_cast<std::intmax_t>(frameOffsetInFile_),
        static_cast<std::intmax_t>(header), static_cast<std::intmax_t>(footer));
    // TODO: error recovery
  }
  positionInRecord = sizeof header;
}

void ExternalFileUnit::BeginVariableFormattedInputRecord(
    IoErrorHandler &handler) {
  if (this == defaultInput) {
    if (defaultOutput) {
      defaultOutput->FlushOutput(handler);
    }
    if (errorOutput) {
      errorOutput->FlushOutput(handler);
    }
  }
  std::size_t length{0};
  do {
    std::size_t need{length + 1};
    length =
        ReadFrame(frameOffsetInFile_, recordOffsetInFrame_ + need, handler) -
        recordOffsetInFrame_;
    if (length < need) {
      if (length > 0) {
        // final record w/o \n
        recordLength = length;
        unterminatedRecord = true;
      } else {
        HitEndOnRead(handler);
      }
      break;
    }
  } while (!SetVariableFormattedRecordLength());
}

void ExternalFileUnit::BackspaceFixedRecord(IoErrorHandler &handler) {
  RUNTIME_CHECK(handler, openRecl.has_value());
  if (frameOffsetInFile_ < *openRecl) {
    handler.SignalError(IostatBackspaceAtFirstRecord);
  } else {
    frameOffsetInFile_ -= *openRecl;
  }
}

void ExternalFileUnit::BackspaceVariableUnformattedRecord(
    IoErrorHandler &handler) {
  std::int32_t header{0};
  auto headerBytes{static_cast<std::int64_t>(sizeof header)};
  frameOffsetInFile_ += recordOffsetInFrame_;
  recordOffsetInFrame_ = 0;
  if (frameOffsetInFile_ <= headerBytes) {
    handler.SignalError(IostatBackspaceAtFirstRecord);
    return;
  }
  // Error conditions here cause crashes, not file format errors, because the
  // validity of the file structure before the current record will have been
  // checked informatively in NextSequentialVariableUnformattedInputRecord().
  std::size_t got{
      ReadFrame(frameOffsetInFile_ - headerBytes, headerBytes, handler)};
  if (static_cast<std::int64_t>(got) < headerBytes) {
    handler.SignalError(IostatShortRead);
    return;
  }
  recordLength = ReadHeaderOrFooter(0);
  if (frameOffsetInFile_ < *recordLength + 2 * headerBytes) {
    handler.SignalError(IostatBadUnformattedRecord);
    return;
  }
  frameOffsetInFile_ -= *recordLength + 2 * headerBytes;
  auto need{static_cast<std::size_t>(
      recordOffsetInFrame_ + sizeof header + *recordLength)};
  got = ReadFrame(frameOffsetInFile_, need, handler);
  if (got < need) {
    handler.SignalError(IostatShortRead);
    return;
  }
  header = ReadHeaderOrFooter(recordOffsetInFrame_);
  if (header != *recordLength) {
    handler.SignalError(IostatBadUnformattedRecord);
    return;
  }
}

// There's no portable memrchr(), unfortunately, and strrchr() would
// fail on a record with a NUL, so we have to do it the hard way.
static const char *FindLastNewline(const char *str, std::size_t length) {
  for (const char *p{str + length}; p >= str; p--) {
    if (*p == '\n') {
      return p;
    }
  }
  return nullptr;
}

void ExternalFileUnit::BackspaceVariableFormattedRecord(
    IoErrorHandler &handler) {
  // File offset of previous record's newline
  auto prevNL{
      frameOffsetInFile_ + static_cast<std::int64_t>(recordOffsetInFrame_) - 1};
  if (prevNL < 0) {
    handler.SignalError(IostatBackspaceAtFirstRecord);
    return;
  }
  while (true) {
    if (frameOffsetInFile_ < prevNL) {
      if (const char *p{
              FindLastNewline(Frame(), prevNL - 1 - frameOffsetInFile_)}) {
        recordOffsetInFrame_ = p - Frame() + 1;
        recordLength = prevNL - (frameOffsetInFile_ + recordOffsetInFrame_);
        break;
      }
    }
    if (frameOffsetInFile_ == 0) {
      recordOffsetInFrame_ = 0;
      recordLength = prevNL;
      break;
    }
    frameOffsetInFile_ -= std::min<std::int64_t>(frameOffsetInFile_, 1024);
    auto need{static_cast<std::size_t>(prevNL + 1 - frameOffsetInFile_)};
    auto got{ReadFrame(frameOffsetInFile_, need, handler)};
    if (got < need) {
      handler.SignalError(IostatShortRead);
      return;
    }
  }
  if (Frame()[recordOffsetInFrame_ + *recordLength] != '\n') {
    handler.SignalError(IostatMissingTerminator);
    return;
  }
  if (*recordLength > 0 &&
      Frame()[recordOffsetInFrame_ + *recordLength - 1] == '\r') {
    --*recordLength;
  }
}

void ExternalFileUnit::DoImpliedEndfile(IoErrorHandler &handler) {
  if (!impliedEndfile_ && direction_ == Direction::Output && IsRecordFile() &&
      access != Access::Direct && leftTabLimit) {
    // Complete partial record after non-advancing write before
    // positioning or closing the unit.  Usually sets impliedEndfile_.
    AdvanceRecord(handler);
  }
  if (impliedEndfile_) {
    impliedEndfile_ = false;
    if (access != Access::Direct && IsRecordFile() && mayPosition()) {
      DoEndfile(handler);
    }
  }
}

void ExternalFileUnit::DoEndfile(IoErrorHandler &handler) {
  if (IsRecordFile() && access != Access::Direct) {
    furthestPositionInRecord =
        std::max(positionInRecord, furthestPositionInRecord);
    if (leftTabLimit) {
      // Last read/write was non-advancing, so AdvanceRecord() was not called.
      leftTabLimit.reset();
      ++currentRecordNumber;
    }
    endfileRecordNumber = currentRecordNumber;
  }
  frameOffsetInFile_ += recordOffsetInFrame_ + furthestPositionInRecord;
  recordOffsetInFrame_ = 0;
  FlushOutput(handler);
  Truncate(frameOffsetInFile_, handler);
  TruncateFrame(frameOffsetInFile_, handler);
  BeginRecord();
  impliedEndfile_ = false;
}

void ExternalFileUnit::CommitWrites() {
  frameOffsetInFile_ +=
      recordOffsetInFrame_ + recordLength.value_or(furthestPositionInRecord);
  recordOffsetInFrame_ = 0;
  BeginRecord();
}

bool ExternalFileUnit::CheckDirectAccess(IoErrorHandler &handler) {
  if (access == Access::Direct) {
    RUNTIME_CHECK(handler, openRecl);
    if (!directAccessRecWasSet_) {
      handler.SignalError(
          "No REC= was specified for a data transfer with ACCESS='DIRECT'");
      return false;
    }
  }
  return true;
}

void ExternalFileUnit::HitEndOnRead(IoErrorHandler &handler) {
  handler.SignalEnd();
  if (IsRecordFile() && access != Access::Direct) {
    endfileRecordNumber = currentRecordNumber;
  }
}

ChildIo &ExternalFileUnit::PushChildIo(IoStatementState &parent) {
  OwningPtr<ChildIo> current{std::move(child_)};
  Terminator &terminator{parent.GetIoErrorHandler()};
  OwningPtr<ChildIo> next{New<ChildIo>{terminator}(parent, std::move(current))};
  child_.reset(next.release());
  return *child_;
}

void ExternalFileUnit::PopChildIo(ChildIo &child) {
  if (child_.get() != &child) {
    child.parent().GetIoErrorHandler().Crash(
        "ChildIo being popped is not top of stack");
  }
  child_.reset(child.AcquirePrevious().release()); // deletes top child
}

int ExternalFileUnit::GetAsynchronousId(IoErrorHandler &handler) {
  if (!mayAsynchronous()) {
    handler.SignalError(IostatBadAsynchronous);
    return -1;
  } else if (auto least{asyncIdAvailable_.LeastElement()}) {
    asyncIdAvailable_.reset(*least);
    return static_cast<int>(*least);
  } else {
    handler.SignalError(IostatTooManyAsyncOps);
    return -1;
  }
}

bool ExternalFileUnit::Wait(int id) {
  if (static_cast<std::size_t>(id) >= asyncIdAvailable_.size() ||
      asyncIdAvailable_.test(id)) {
    return false;
  } else {
    if (id == 0) { // means "all IDs"
      asyncIdAvailable_.set();
      asyncIdAvailable_.reset(0);
    } else {
      asyncIdAvailable_.set(id);
    }
    return true;
  }
}

std::int32_t ExternalFileUnit::ReadHeaderOrFooter(std::int64_t frameOffset) {
  std::int32_t word;
  char *wordPtr{reinterpret_cast<char *>(&word)};
  std::memcpy(wordPtr, Frame() + frameOffset, sizeof word);
  if (swapEndianness_) {
    SwapEndianness(wordPtr, sizeof word, sizeof word);
  }
  return word;
}

void ChildIo::EndIoStatement() {
  io_.reset();
  u_.emplace<std::monostate>();
}

Iostat ChildIo::CheckFormattingAndDirection(
    bool unformatted, Direction direction) {
  bool parentIsInput{!parent_.get_if<IoDirectionState<Direction::Output>>()};
  bool parentIsFormatted{parentIsInput
          ? parent_.get_if<FormattedIoStatementState<Direction::Input>>() !=
              nullptr
          : parent_.get_if<FormattedIoStatementState<Direction::Output>>() !=
              nullptr};
  bool parentIsUnformatted{!parentIsFormatted};
  if (unformatted != parentIsUnformatted) {
    return unformatted ? IostatUnformattedChildOnFormattedParent
                       : IostatFormattedChildOnUnformattedParent;
  } else if (parentIsInput != (direction == Direction::Input)) {
    return parentIsInput ? IostatChildOutputToInputParent
                         : IostatChildInputFromOutputParent;
  } else {
    return IostatOk;
  }
}

} // namespace Fortran::runtime::io
