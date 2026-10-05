# Build-time source patches for the pinned Nestopia core.
# The submodule stays byte-identical to upstream; patched copies of individual files are
# generated into the build tree and replace the originals in the source list. Every patch is an
# exact-text replacement that MUST match exactly once, otherwise configuration fails, so a core
# bump can never silently drop a fix. Bump REPLAYNES_NESTOPIA_PATCHLEVEL whenever this list
# changes: it is part of the core compatibility ID (docs/COMPATIBILITY.md).
set(REPLAYNES_NESTOPIA_PATCHLEVEL 1)
set(_rn_patch_dir ${CMAKE_BINARY_DIR}/nestopia-patched)
set(_rn_patches 1 2 3 4)

# --- Patch 1 (NstApu.cpp, LoadState) ---------------------------------------------------------
# LoadState dropped the scheduled frame-sequencer clock whenever the frame IRQ was inhibited
# ($4017 = $40, 4-step). WriteFrameCtrl/Reset keep it running in that mode (it drives the $4015
# bit-6 "phantom" flag), so a loaded machine differed from the uninterrupted run (different state
# hash and $4015 reads). Only 5-step mode stops the clock.
set(_rn_p1_file core/NstApu.cpp)
set(_rn_p1_old [=[			if (ctrl != STATUS_FRAME_IRQ_ENABLE)
			{
				cycles.frameIrqClock = Cpu::CYCLE_MAX;
				cycles.frameIrqRepeat = 0;
			}]=])
set(_rn_p1_new [=[			if (ctrl & STATUS_SEQUENCE_5_STEP) /* ReplayNES patch 1: keep the 4-step clock when only inhibited */
			{
				cycles.frameIrqClock = Cpu::CYCLE_MAX;
				cycles.frameIrqRepeat = 0;
			}]=])

# --- Patch 2 (NstApu.cpp, SaveState) ---------------------------------------------------------
# The APU output stage (resampler accumulators and the samples already produced but not yet
# delivered) was not part of the state. A loaded machine therefore emitted PCM shifted by ~1
# sample and its channel timers drifted apart in representation (different state hash). Persist
# them in an extra "RNA" chunk (ignored by upstream loaders).
set(_rn_p2_file core/NstApu.cpp)
set(_rn_p2_old [=[				state.Begin( AsciiId<'S','0','0'>::V ).Write( data ).End();
			}

			state.End();
		}

		void Apu::LoadState(State::Loader& state)]=])
set(_rn_p2_new [=[				state.Begin( AsciiId<'S','0','0'>::V ).Write( data ).End();
			}

			{
				/* ReplayNES patch 2: carry the output stage across a state. Draining and
				 * re-filling the ring only normalises its positions; contents are unchanged. */
				Sound::Buffer& ring = const_cast<Sound::Buffer&>(buffer);
				Sound::Buffer::Block block( Sound::Buffer::SIZE );
				ring >> block;
				const uint count = block.length;
				iword pending[Sound::Buffer::SIZE]; /* local: no state shared between instances */
				for (uint i=0; i < count; ++i)
					pending[i] = block.data[(block.start + i) & Sound::Buffer::MASK];
				for (uint i=0; i < count; ++i)
					ring << pending[i];

				state.Begin( AsciiId<'R','N','A'>::V );
				state.Write64( cycles.sampleSum ).Write64( cycles.sampleNext ).Write32( cycles.sampleSpan ).Write16( count );
				for (uint i=0; i < count; ++i)
					state.Write16( uint(pending[i]) & 0xFFFFU );
				state.End();
			}

			state.End();
		}

		void Apu::LoadState(State::Loader& state)]=])

# --- Patch 3 (NstApu.cpp, LoadState) ---------------------------------------------------------
set(_rn_p3_file core/NstApu.cpp)
set(_rn_p3_old [=[						cycles.rateCounter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
						cycles.sampleSum = 0;
						cycles.sampleNext = 0;
						cycles.sampleSpan = 0;
						break;
					}]=])
set(_rn_p3_new [=[						cycles.rateCounter = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
						cycles.sampleSum = 0;
						cycles.sampleNext = 0;
						cycles.sampleSpan = 0;
						break;
					}

					case AsciiId<'R','N','A'>::V: /* ReplayNES patch 2/3: restore the output stage */
					{
						const qaword sum = state.Read64();
						const qaword next = state.Read64();
						const dword span = state.Read32();
						const uint count = state.Read16();

						buffer.Reset( false );

						for (uint i=0; i < count && i < Sound::Buffer::SIZE - 1; ++i)
						{
							const int v = int(state.Read16());
							buffer << Sound::Sample( v >= 0x8000 ? v - 0x10000 : v );
						}

						cycles.sampleSum = sum;
						cycles.sampleNext = next;
						cycles.sampleSpan = span;
						break;
					}]=])

# --- Patch 4 (NstApu.cpp, Triangle ctor) -----------------------------------------------------
# Triangle::linearCtrl ($4008 control) is deliberately kept across resets, but it was never
# initialised, so a fresh instance's power-on state depended on heap garbage (found by comparing
# fresh instances' state hashes). Zero it once at construction.
set(_rn_p4_file core/NstApu.cpp)
set(_rn_p4_old [=[		Apu::Triangle::Triangle()
		: outputVolume(0) {}]=])
set(_rn_p4_new [=[		Apu::Triangle::Triangle()
		: outputVolume(0) { linearCtrl = 0; /* ReplayNES patch 4: no uninitialised power-on state */ }]=])

# --- apply ------------------------------------------------------------------------------------
set(REPLAYNES_NESTOPIA_PATCHED_FILES "")
foreach(_i IN LISTS _rn_patches)
  set(_f ${_rn_p${_i}_file})
  string(MAKE_C_IDENTIFIER "${_f}" _key)
  if(NOT DEFINED _rn_text_${_key})
    file(READ ${NST_ROOT}/${_f} _rn_text_${_key})
    list(APPEND REPLAYNES_NESTOPIA_PATCHED_FILES ${_f})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NST_ROOT}/${_f})
  endif()
  set(_text "${_rn_text_${_key}}")
  string(FIND "${_text}" "${_rn_p${_i}_old}" _pos)
  if(_pos EQUAL -1)
    message(FATAL_ERROR "Nestopia patch ${_i} does not apply to ${_f} (core changed?)")
  endif()
  string(REPLACE "${_rn_p${_i}_old}" "${_rn_p${_i}_new}" _patched "${_text}")
  string(LENGTH "${_text}" _l0)
  string(LENGTH "${_rn_p${_i}_old}" _lo)
  string(LENGTH "${_rn_p${_i}_new}" _ln)
  string(LENGTH "${_patched}" _l1)
  math(EXPR _expect "${_l0} - ${_lo} + ${_ln}")
  if(NOT _l1 EQUAL _expect)
    message(FATAL_ERROR "Nestopia patch ${_i} matched more than once in ${_f}")
  endif()
  set(_rn_text_${_key} "${_patched}")
endforeach()
foreach(_f IN LISTS REPLAYNES_NESTOPIA_PATCHED_FILES)
  string(MAKE_C_IDENTIFIER "${_f}" _key)
  file(GENERATE OUTPUT ${_rn_patch_dir}/${_f} CONTENT "${_rn_text_${_key}}")
  list(REMOVE_ITEM NST_CORE_SOURCES ${NST_ROOT}/${_f})
  list(APPEND NST_CORE_SOURCES ${_rn_patch_dir}/${_f})
endforeach()
