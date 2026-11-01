# Narrow native-playback fixes from derselbst/amuse:
# 9e34e4c87a1adbf7edd666bca5885d3c5ec99b9c (MusyX extended controls)
# a7f7d64e8f83253e134ef3bdf0aecbdc557a4d2b (pitch wheel and RPN defaults)
# Only these native engine files are changed; the later FluidSynth exporter and
# song-converter refactors are not imported. The final call-site adjustment uses
# the official standalone Nowide API rather than an emulated conversion header.
# Input and result text are authenticated after CRLF normalization, so the
# checks are independent of native file-write line endings. Reconfiguration is
# idempotent.
include("${CMAKE_CURRENT_LIST_DIR}/GdlPatchAmuseEffects.cmake")

function(gdl_patch_amuse_file source relative original_hash patched_hash)
    set(path "${source}/${relative}")
    file(READ "${path}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    string(SHA256 actual_hash "${content}")
    if(actual_hash STREQUAL patched_hash)
        return()
    endif()
    if(NOT actual_hash IN_LIST original_hash)
        message(FATAL_ERROR "Unexpected Amuse source bytes: ${relative}")
    endif()
    set(index 4)
    while(index LESS ARGC)
        set(before "${ARGV${index}}")
        math(EXPR index "${index} + 1")
        set(after "${ARGV${index}}")
        math(EXPR index "${index} + 1")
        string(FIND "${content}" "${before}" offset)
        if(offset LESS 0)
            # Authenticated intermediate revisions may already have applied
            # this replacement or changed its context in a later replacement.
            # The mandatory final full-file hash still gates the entire result;
            # missing contexts never certify success.
        else()
            string(REPLACE "${before}" "${after}" content "${content}")
        endif()
    endwhile()
    string(SHA256 actual_hash "${content}")
    if(NOT actual_hash STREQUAL patched_hash)
        message(FATAL_ERROR "Amuse patch result differs: ${relative}")
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

function(gdl_patch_amuse source)
    gdl_patch_amuse_file("${source}" "include/amuse/Sequencer.hpp"
        8bc5f3fc65cad5ec5d9c72993ae6ee06879307cb2222f9e4b4ac8452f93b3ffe
        a3d9dcb4d2048eab10913f3b11dec3f7965e03aeb0cdc4552b0d0a3b3b265c84
        [=[    std::unordered_map<uint8_t, ObjToken<Voice>> m_chanVoxs;
    std::unordered_set<ObjToken<Voice>> m_keyoffVoxs;
    ObjToken<Voice> m_lastVoice;
    std::array<int8_t, 128> m_ctrlVals{}; /**< MIDI controller values */
    float m_curPitchWheel = 0.f;          /**< MIDI pitch-wheel */
    int8_t m_pitchWheelRange = -1;        /**< Pitch wheel range settable by RPN 0 */
    int8_t m_curProgram = 0;              /**< MIDI program number */
    float m_curVol = 1.f;                 /**< Current volume of channel */
    float m_curPan = 0.f;                 /**< Current panning of channel */
    uint16_t m_rpn = 0;                   /**< Current RPN (only pitch-range 0x0000 supported) */
    double m_ticksPerSec = 1000.0;        /**< Current ticks per second (tempo) for channel */

    void _bringOutYourDead();
    size_t getVoiceCount() const;
    ObjToken<Voice> keyOn(uint8_t note, uint8_t velocity);
    void keyOff(uint8_t note, uint8_t velocity);
    void setCtrlValue(uint8_t ctrl, int8_t val);
    bool programChange(int8_t prog);
    void nextProgram();
    void prevProgram();
]=]
        [=[    std::unordered_map<uint8_t, ObjToken<Voice>> m_chanVoxs;
    std::unordered_set<ObjToken<Voice>> m_keyoffVoxs;
    ObjToken<Voice> m_lastVoice;
    std::array<int8_t, 134> m_ctrlVals{}; /**< MIDI controller values (134 to match MusyX extended CCs) */
    float m_curPitchWheel = 0.f;          /**< MIDI pitch-wheel */
    int8_t m_pitchWheelRange = -1;        /**< Pitch wheel range settable by RPN 0 */
    int8_t m_curProgram = 0;              /**< MIDI program number */
    float m_curVol = 1.f;                 /**< Current volume of channel */
    float m_curPan = 0.f;                 /**< Current panning of channel */
    uint16_t m_rpn = 0x3FFF;              /**< Current RPN; 0x3FFF = null (no parameter selected, matching MusyX cold defaults) */
    double m_ticksPerSec = 1000.0;        /**< Current ticks per second (tempo) for channel */

    void _bringOutYourDead();
    size_t getVoiceCount() const;
    ObjToken<Voice> keyOn(uint8_t note, uint8_t velocity);
    void keyOff(uint8_t note, uint8_t velocity);
    void setCtrlValue(uint16_t ctrl, int8_t val);
    bool programChange(int8_t prog);
    void nextProgram();
    void prevProgram();
]=]
        [=[  void keyOff(uint8_t chan, uint8_t note, uint8_t velocity);

  /** Set MIDI control value [0,127] for all voices */
  void setCtrlValue(uint8_t chan, uint8_t ctrl, int8_t val);

  /** Set pitchwheel value for use with voice controllers */
  void setPitchWheel(uint8_t chan, float pitchWheel);
]=]
        [=[  void keyOff(uint8_t chan, uint8_t note, uint8_t velocity);

  /** Set MIDI control value [0,127] for all voices */
  void setCtrlValue(uint8_t chan, uint16_t ctrl, int8_t val);

  /** Set pitchwheel value for use with voice controllers */
  void setPitchWheel(uint8_t chan, float pitchWheel);
]=]
    )
    gdl_patch_amuse_file("${source}" "include/amuse/Voice.hpp"
        c3ffe925286293b343ac8dabcbdc0eda136aac94b22745537a09577b1f036e9c
        80c1f787b5c7bc8ea0f392adce6f467bccf80b39e3b22bda60804c2699769056
        [=[  std::unique_ptr<int8_t[]> m_ctrlValsSelf; /**< Self-owned MIDI Controller values */
  int8_t* m_extCtrlVals = nullptr;          /**< MIDI Controller values (external storage) */

  uint16_t m_rpn = 0; /**< Current RPN (only pitch-range 0x0000 supported) */

  void _destroy();
  bool _checkSamplePos(bool& looped);
]=]
        [=[  std::unique_ptr<int8_t[]> m_ctrlValsSelf; /**< Self-owned MIDI Controller values */
  int8_t* m_extCtrlVals = nullptr;          /**< MIDI Controller values (external storage) */

  uint16_t m_rpn = 0x3FFF; /**< Current RPN; 0x3FFF = null (no parameter selected, matching MusyX cold defaults) */

  void _destroy();
  bool _checkSamplePos(bool& looped);
]=]
        [=[  void _setSurroundPan(float span);
  void _setChannelCoefs(const std::array<float, 8>& coefs);
  void _setPitchWheel(float pitchWheel);
  void _notifyCtrlChange(uint8_t ctrl, int8_t val);

public:
  ~Voice() override;
]=]
        [=[  void _setSurroundPan(float span);
  void _setChannelCoefs(const std::array<float, 8>& coefs);
  void _setPitchWheel(float pitchWheel);
  void _notifyCtrlChange(uint16_t ctrl, int8_t val);

public:
  ~Voice() override;
]=]
        [=[  bool doPortamento(uint8_t newNote);

  /** Get MIDI Controller value on voice */
  int8_t getCtrlValue(uint8_t ctrl) const {
    if (!m_extCtrlVals) {
      if (m_ctrlValsSelf)
        return m_ctrlValsSelf[ctrl];
      return 0;
    }
    return m_extCtrlVals[ctrl];
  }

  /** Set MIDI Controller value on voice */
  void setCtrlValue(uint8_t ctrl, int8_t val) {
    if (!m_extCtrlVals) {
      std::unique_ptr<int8_t[]>& vals = _ensureCtrlVals();
      vals[ctrl] = val;
]=]
        [=[  bool doPortamento(uint8_t newNote);

  /** Get MIDI Controller value on voice */
  int8_t getCtrlValue(uint16_t ctrl) const {
    if (!m_extCtrlVals) {
      if (m_ctrlValsSelf && ctrl < 134)
        return m_ctrlValsSelf[ctrl];
      return 0;
    }
    if (ctrl < 134)
      return m_extCtrlVals[ctrl];
    return 0;
  }

  /** Set MIDI Controller value on voice */
  void setCtrlValue(uint16_t ctrl, int8_t val) {
    if (ctrl >= 134)
      return;
    if (!m_extCtrlVals) {
      std::unique_ptr<int8_t[]>& vals = _ensureCtrlVals();
      vals[ctrl] = val;
]=]
    )
    gdl_patch_amuse_file("${source}" "lib/Sequencer.cpp"
        581eae7b6f40f8712df5517c8a3fdee4bd9d648f4ea9d530a4065d58e5fc46fa
        92a07ad4d2dc4260de66c45df7de7706029fd4a4ec897dada766d54a4d4b5a60
        [=[  m_chanStates[chan].keyOff(note, velocity);
}

void Sequencer::ChannelState::setCtrlValue(uint8_t ctrl, int8_t val) {
  m_ctrlVals[ctrl] = val;
  for (const auto& vox : m_chanVoxs)
    vox.second->_notifyCtrlChange(ctrl, val);
]=]
        [=[  m_chanStates[chan].keyOff(note, velocity);
}

void Sequencer::ChannelState::setCtrlValue(uint16_t ctrl, int8_t val) {
  if (ctrl >= m_ctrlVals.size())
    return;
  m_ctrlVals[ctrl] = val;
  for (const auto& vox : m_chanVoxs)
    vox.second->_notifyCtrlChange(ctrl, val);
]=]
        [=[  case 10:
    setPan(val / 64.f - 1.f);
    break;
  case 98:
    // RPN LSB
    m_rpn &= ~0x7f;
    m_rpn |= val;
    break;
  case 99:
    // RPN MSB
    m_rpn &= ~0x3f80;
    m_rpn |= val << 7;
]=]
        [=[  case 10:
    setPan(val / 64.f - 1.f);
    break;
  case 100:
    // RPN LSB
    m_rpn &= ~0x7f;
    m_rpn |= val;
    break;
  case 101:
    // RPN MSB
    m_rpn &= ~0x3f80;
    m_rpn |= val << 7;
]=]
        [=[      break;
}

void Sequencer::setCtrlValue(uint8_t chan, uint8_t ctrl, int8_t val) {
  if (chan >= m_chanStates.size()) {
    return;
  }
]=]
        [=[      break;
}

void Sequencer::setCtrlValue(uint8_t chan, uint16_t ctrl, int8_t val) {
  if (chan >= m_chanStates.size()) {
    return;
  }
]=]
    )
    gdl_patch_amuse_file("${source}" "lib/SongState.cpp"
        "b02db28b1851159f54f80147427494fda2d106ec2f1e77f60576e39cf6bf9e93;d36409cfe36d683b18a13859b60a1533285a9f6c51f564b4f4dfa1f47dd77f6a;f712eddd13fb96b929f30e2a236a46792bbefd48ae1b63d53d8ccc2b0727408a;f8a87b97531ec5908c6acfa7c7a5d288252f9be5acb2f80e17f9d8bee18a8321"
        59529f78564fca434703fe03b3985d7e385151cfbbb548ced623bbed5c779962
        [=[          m_data = nullptr;
          break;
        }
        if ((m_data[0] & 0x80) != 0u && (m_data[1] & 0x80) != 0u) {
          /* Control change */
          uint8_t val = m_data[0] & 0x7f;
          uint8_t ctrl = m_data[1] & 0x7f;
          seq.setCtrlValue(m_midiChan, ctrl, static_cast<int8_t>(val));
          m_data += 2;
        } else if ((m_data[0] & 0x80) != 0u) {
          /* Program change */
          uint8_t prog = m_data[0] & 0x7f;
          seq.setChanProgram(static_cast<int8_t>(m_midiChan), static_cast<int8_t>(prog));
          m_data += 2;
        } else {
          /* Note */
]=]
        [=[          m_data = nullptr;
          break;
        }
        if ((m_data[0] & 0x80) != 0u) {
          /* Special event: key byte has bit 7 set.
           * Match the original MusyX SDK HandleEvent (seq.c) encoding:
           *   velocity == 0:       Program change  (prog = key & 0x7f)
           *   velocity == 1:       CC 0x82 (130)   (val  = key & 0x7f)
           *   velocity & 0x80:     CC              (ctrl = velocity & 0x7f, val = key & 0x7f)
           *   velocity 2..127:     ignored / internal meta-command
           */
          uint8_t key = m_data[0];
          uint8_t vel = m_data[1];
          if (vel == 0) {
            /* Program change */
            uint8_t prog = key & 0x7f;
            seq.setChanProgram(static_cast<int8_t>(m_midiChan), static_cast<int8_t>(prog));
          } else if (vel == 1) {
            /* MusyX extended CC 0x82 (130) */
            uint8_t val = key & 0x7f;
            seq.setCtrlValue(m_midiChan, 0x82, static_cast<int8_t>(val));
          } else if ((vel & 0x80) != 0) {
            /* Standard CC: ctrl = vel & 0x7f, val = key & 0x7f */
            uint8_t ctrl = vel & 0x7f;
            uint8_t val  = key & 0x7f;
            seq.setCtrlValue(m_midiChan, ctrl, static_cast<int8_t>(val));
          }
          /* else: velocity 2..127 → ignored (internal MusyX meta-commands) */
          m_data += 2;
        } else {
          /* Note */
]=]
        [=[          m_data = nullptr;
          break;
        }
        if ((m_data[2] & 0x80) != 0x80) {
          /* Note */
          uint16_t length = (m_parent->m_bigEndian ? SBig(*reinterpret_cast<const uint16_t*>(m_data))
                                                   : *reinterpret_cast<const uint16_t*>(m_data));
          uint8_t note = m_data[2] & 0x7f;
]=]
        [=[          m_data = nullptr;
          break;
        }
        if ((m_data[2] & 0x80) == 0) {
          /* Note: key byte has bit 7 clear */
          uint16_t length = (m_parent->m_bigEndian ? SBig(*reinterpret_cast<const uint16_t*>(m_data))
                                                   : *reinterpret_cast<const uint16_t*>(m_data));
          uint8_t note = m_data[2] & 0x7f;
]=]
        [=[            seq.keyOff(m_midiChan, note, 0);
          }
          m_remNoteLengths[note] = length;
        } else if ((m_data[2] & 0x80) != 0u && (m_data[3] & 0x80) != 0u) {
          /* Control change */
          uint8_t val = m_data[2] & 0x7f;
          uint8_t ctrl = m_data[3] & 0x7f;
          seq.setCtrlValue(m_midiChan, ctrl, static_cast<int8_t>(val));
        } else if ((m_data[2] & 0x80) != 0u) {
          /* Program change */
          uint8_t prog = m_data[2] & 0x7f;
          seq.setChanProgram(static_cast<int8_t>(m_midiChan), static_cast<int8_t>(prog));
        }
        m_data += 4;

]=]
        [=[            seq.keyOff(m_midiChan, note, 0);
          }
          m_remNoteLengths[note] = length;
        } else {
          /* Special event: key byte has bit 7 set.
           * Match the original MusyX SDK HandleEvent (seq.c) encoding:
           *   velocity == 0:       Program change  (prog = key & 0x7f)
           *   velocity == 1:       CC 0x82 (130)   (val  = key & 0x7f)
           *   velocity & 0x80:     CC              (ctrl = velocity & 0x7f, val = key & 0x7f)
           *   velocity 2..127:     ignored / internal meta-command
           */
          uint8_t key = m_data[2];
          uint8_t vel = m_data[3];
          if (vel == 0) {
            /* Program change */
            uint8_t prog = key & 0x7f;
            seq.setChanProgram(static_cast<int8_t>(m_midiChan), static_cast<int8_t>(prog));
          } else if (vel == 1) {
            /* MusyX extended CC 0x82 (130) */
            uint8_t val = key & 0x7f;
            seq.setCtrlValue(m_midiChan, 0x82, static_cast<int8_t>(val));
          } else if ((vel & 0x80) != 0) {
            /* Standard CC: ctrl = vel & 0x7f, val = key & 0x7f */
            uint8_t ctrl = vel & 0x7f;
            uint8_t val  = key & 0x7f;
            seq.setCtrlValue(m_midiChan, ctrl, static_cast<int8_t>(val));
          }
          /* else: velocity 2..127 → ignored (internal MusyX meta-commands) */
        }
        m_data += 4;

]=]
        # Keep scheduled region boundaries and tempo changes accurate. The
        # final replacement below separates a looped pattern's activation
        # clock from its earlier authored arrangement start.
        [=[  m_curRegion = region;
  uint32_t regionIdx]=]
        [=[  m_curRegion = region;
  const uint32_t regionTick = m_parent->m_bigEndian ? SBig(region->m_startTick) : region->m_startTick;
  uint32_t regionIdx]=]
        [=[m_nextPitchTick = m_curTick + delta.first;]=]
        [=[m_nextPitchTick = regionTick + delta.first;]=]
        [=[m_nextModTick = m_curTick + delta.first;]=]
        [=[m_nextModTick = regionTick + delta.first;]=]
        [=[void SongState::Track::resetTempo() {
  if (m_parent->m_header.m_tempoTableOff != 0u) {
    m_tempoPtr = reinterpret_cast<const TempoChange*>(m_parent->m_songData + m_parent->m_header.m_tempoTableOff);
  } else {
    m_tempoPtr = nullptr;
  }
}]=]
        [=[void SongState::Track::resetTempo() {
  m_tempo = m_parent->m_header.m_initialTempo & 0x7fffffff;
  if (m_parent->m_header.m_tempoTableOff != 0u) {
    m_tempoPtr = reinterpret_cast<const TempoChange*>(m_parent->m_songData + m_parent->m_header.m_tempoTableOff);
    while (m_tempoPtr->m_tick != 0xffffffff) {
      TempoChange change = *m_tempoPtr;
      if (m_parent->m_bigEndian) change.swapBig();
      if (change.m_tick > m_curTick) break;
      m_tempo = change.m_tempo & 0x7fffffff;
      ++m_tempoPtr;
    }
  } else {
    m_tempoPtr = nullptr;
  }
}]=]
        [=[  double ticksPerSecond = static_cast<double>(m_tempo) * 384 / 60;
  uint32_t ticks]=]
        [=[  double ticksPerSecond = static_cast<double>(m_tempo) * 384 / 60;
  seq.setTempo(m_midiChan, ticksPerSecond);
  uint32_t ticks]=]
        [=[    m_lastN64EventTick = absTick;
    m_data += 4;
  }
}]=]
        [=[    m_lastN64EventTick = absTick;
    m_data += 4;
  }
  m_eventWaitCountdown += int32_t(regionTick) - int32_t(m_curTick);
}]=]
        [=[          seq.keyOn(m_midiChan, note, vel);
          if (length == 0) {
            seq.keyOff(m_midiChan, note, 0);
          }
          m_remNoteLengths[note] = length;]=]
        [=[          const int remaining = int(length) + std::min(m_eventWaitCountdown, 0);
          if (remaining > 0 || (length == 0 &&
              m_eventWaitCountdown >= -int32_t(endTick - m_curTick))) {
            seq.keyOn(m_midiChan, note, vel);
            if (remaining <= 0) {
              seq.keyOff(m_midiChan, note, 0);
            }
            m_remNoteLengths[note] = std::max(remaining, 0);
          }]=]
        [=[      if (uint32_t(endTick) > loopEndTick) {
        m_nextRegion]=]
        [=[      if (uint32_t(endTick) >= loopEndTick) {
        // Carry the fractional update past the loop boundary. Discarding it
        // makes independent tracks drift apart on each repetition.
        m_remDt += (endTick - loopEndTick) / ticksPerSecond;
        m_nextRegion]=]
        [=[    if (ticks <= 0) {
      /* Turn over tempo */]=]
        [=[    if (m_curTick >= change.m_tick) {
      /* Turn over tempo */]=]
        [=[  m_remDt -= ticks / ticksPerSecond;]=]
        [=[  if (m_parent->m_loop) {
    const TrackRegion* terminator = m_nextRegion;
    while (terminator->indexValid(m_parent->m_bigEndian)) ++terminator;
    if (terminator->indexLoop(m_parent->m_bigEndian) >= 0) {
      const uint32_t loopEnd = m_parent->m_bigEndian ? SBig(terminator->m_startTick) : terminator->m_startTick;
      if (loopEnd >= m_curTick) ticks = std::min(ticks, loopEnd - m_curTick);
    }
  }
  m_remDt -= ticks / ticksPerSecond;]=]
        [=[        m_remDt += (endTick - loopEndTick) / ticksPerSecond;
]=]
        [=[]=]
        [=[  const uint32_t regionTick = m_parent->m_bigEndian ? SBig(region->m_startTick) : region->m_startTick;]=]
        [=[  const uint32_t scheduledTick = m_parent->m_bigEndian ? SBig(region->m_startTick) : region->m_startTick;
  // N64 MusyX resets the pattern clock independently of the arrangement clock
  // (game.bin e0081cdc / e0081300 / e0081760). A loop may select a region
  // authored before the loop point; its notes restart, rather than being sought
  // past. cathy2a uses this to repeat its organ phrase over the later backing.
  const uint32_t regionTick = std::max(scheduledTick, m_curTick);]=]
    )
    gdl_patch_amuse_file("${source}" "lib/Voice.cpp"
        "8e4d642fb016dbee180a84ef2ed4b63ca7261b66ef3adb4fddda192f1aea210f;0ec9acab898403ccb1e5f97eb9d8c3fe1019b8b89f7b823076722efac97c64a5"
        d84fd1df609e65824c3dd14e2a637f240b7687862b585ded0d1f08c93353d68a
        [=[    return m_ctrlValsSelf;
  }

  m_ctrlValsSelf = std::make_unique<int8_t[]>(128);
  return m_ctrlValsSelf;
}

]=]
        [=[    return m_ctrlValsSelf;
  }

  m_ctrlValsSelf = std::make_unique<int8_t[]>(134);
  return m_ctrlValsSelf;
}

]=]
        [=[    return;

  if (pitchWheel > 0.f)
    m_pitchWheelVal = m_pitchWheelUp * m_curPitchWheel;
  else if (pitchWheel < 0.f)
    m_pitchWheelVal = m_pitchWheelDown * m_curPitchWheel;
  else
    m_pitchWheelVal = 0;
  m_pitchDirty = true;
]=]
        [=[    return;

  if (pitchWheel > 0.f)
    m_pitchWheelVal = m_pitchWheelUp * pitchWheel;
  else if (pitchWheel < 0.f)
    m_pitchWheelVal = m_pitchWheelDown * pitchWheel;
  else
    m_pitchWheelVal = 0;
  m_pitchDirty = true;
]=]
        [=[  return true;
}

void Voice::_notifyCtrlChange(uint8_t ctrl, int8_t val) {
  if (ctrl == 0x40) {
    setPedal(val >= 0x40);
  } else if (ctrl == 0x5b) {
]=]
        [=[  return true;
}

void Voice::_notifyCtrlChange(uint16_t ctrl, int8_t val) {
  if (ctrl == 0x40) {
    setPedal(val >= 0x40);
  } else if (ctrl == 0x5b) {
]=]
        [=[  evalVol *= m_state.m_reverbSel ? (m_state.m_reverbSel.evaluate(time, *this, m_state) / 127.f) : m_curReverbVol;]=]
        [=[  evalVol *= m_state.m_reverbSel ? (m_state.m_reverbSel.evaluate(time, *this, m_state) / 127.f) : m_curReverbVol;
  evalVol = m_state.applyReverbLevel(evalVol);]=]
    )
    gdl_patch_amuse_file("${source}" "lib/AudioGroupSampleDirectory.cpp"
        e72ca88a2a32e7bfe60d3f173dbfcbf065a1e0b72cf29cf9d9191eb07a711c68
        db10beadaa3ee7ecabd99e9797b320ae07cbbe35529083fad662d1a9f056b0c7
        [=[const nowide::wstackstring wpath(path);]=]
        [=[const nowide::wstackstring wpath(path.c_str());]=]
    )
    gdl_patch_amuse_effects("${source}")
endfunction()
