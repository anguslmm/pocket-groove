// Report formatting (see report.h). Portable, host-tested.
#include "report.h"

#include <cstdarg>
#include <cstdio>

namespace hws {

namespace {

class Emitter {
 public:
  Emitter(LineSink sink, void* user) : sink_(sink), user_(user) {}
  void line(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    char buf[kReportLineMax + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (sink_) sink_(buf, user_);
    ++lines_;
  }
  int lines() const { return lines_; }

 private:
  LineSink sink_;
  void* user_;
  int lines_ = 0;
};

// A float, or "na" when not measured (negative sentinel or !valid).
struct F {
  char s[24];
  F(float v, int prec, bool valid = true) {
    if (!valid || v != v) snprintf(s, sizeof(s), "na");
    else snprintf(s, sizeof(s), "%.*f", prec, static_cast<double>(v));
  }
};
// Non-negative quantity where -1 means "not measured".
inline F N(float v, int prec) { return F(v, prec, v >= 0.0f); }

inline unsigned long U(uint32_t v) { return static_cast<unsigned long>(v); }

}  // namespace

int formatTiltReport(const TiltReport& r, LineSink sink, void* user) {
  Emitter e(sink, user);
  e.line("===== PG-HWSPIKE TILT BEGIN %s =====", kReportVersion);
  e.line("imu: ok=%d addr=0x%02X chip=0x%02X err=0x%02X acc_conf=0x%02X acc_range=0x%02X gyr_conf=0x%02X "
         "gyr_range=0x%02X pwr_conf=0x%02X pwr_ctrl=0x%02X",
         r.imuOk ? 1 : 0, r.addr, r.chipId, r.errReg, r.accConf, r.accRange, r.gyrConf, r.gyrRange, r.pwrConf,
         r.pwrCtrl);
  e.line("scale: acc_lsb_per_g=%s gyr_lsb_per_dps=%s", F(r.accLsbPerG, 1).s, F(r.gyrLsbPerDps, 2).s);
  e.line("rate: set_hz=%lu poll_hz=%s new_hz=%s dup_pct=%s gaps=%lu odr_sensortime_hz=%s ticks_per_sample=%s",
         U(r.setHz), F(r.pollHz, 1).s, F(r.newHz, 1).s, F(r.dupPct, 1).s, U(r.gaps), F(r.odrSensortimeHz, 1).s,
         F(r.ticksPerSample, 2).s);
  e.line("i2c: burst18_avg_us=%s burst18_max_us=%s read1_us=%s errors=%lu", F(r.i2cAvgUs, 1).s,
         F(r.i2cMaxUs, 1).s, F(r.i2c1ByteUs, 1).s, U(r.i2cErrors));
  e.line("cpu: imu_us_per_poll=%s math_cyc_per_sample=%s imu_core1_pct=%s audio_avg_us=%s audio_max_us=%s "
         "audio_core0_pct=%s",
         F(r.taskUsPerPoll, 1).s, F(r.mathCyclesPerSample, 0).s, F(r.imuCorePct, 2).s, F(r.audioAvgUs, 1).s,
         F(r.audioMaxUs, 1).s, F(r.audioCpuPct, 1).s);
  e.line("noise: acc_a_deg=%s acc_b_deg=%s fused_a_deg=%s fused_b_deg=%s gyro_dps=%s (std, quietest 2 s window)",
         N(r.noiseAccA, 3).s, N(r.noiseAccB, 3).s, N(r.noiseFusedA, 3).s, N(r.noiseFusedB, 3).s,
         N(r.gyroNoiseDps, 3).s);
  e.line("rest: acc_g=%s,%s,%s norm_g=%s bias_dps=%s,%s,%s calibrated=%d", F(r.accMeanG[0], 3).s,
         F(r.accMeanG[1], 3).s, F(r.accMeanG[2], 3).s, F(r.accMeanNormG, 3).s, F(r.biasDps[0], 3).s,
         F(r.biasDps[1], 3).s, F(r.biasDps[2], 3).s, r.biasCalibrated ? 1 : 0);
  e.line("drift: secs=%s gyro_only_deg=%s fused_vs_acc_deg=%s gyro_deg_per_min=%s", F(r.driftSecs, 1).s,
         N(r.driftGyroDeg, 3).s, N(r.driftFusedDeg, 3).s,
         F(r.driftSecs > 1.0f ? r.driftGyroDeg * 60.0f / r.driftSecs : -1.0f, 3, r.driftSecs > 1.0f && r.driftGyroDeg >= 0.0f).s);
  e.line("fusion: gyro_sign=%s sign_check=%s (+1 agree, -1 flipped, 0 no motion)", F(r.gyroSign, 0).s,
         F(r.signCheck, 2).s);
  const float base = r.latReadAgeMs + r.latBlockMs + r.latDmaMs;
  e.line("latency_ms: read_age=%s block=%s dma=%s smooth_p1=%s p2=%s p3=%s p4=%s total_p1=%s total_p4=%s",
         F(r.latReadAgeMs, 2).s, F(r.latBlockMs, 2).s, F(r.latDmaMs, 2).s, F(r.latSmoothMs[0], 2).s,
         F(r.latSmoothMs[1], 2).s, F(r.latSmoothMs[2], 2).s, F(r.latSmoothMs[3], 2).s,
         F(base + r.latSmoothMs[0], 2).s, F(base + r.latSmoothMs[3], 2).s);
  e.line("keys_imu_off: n=%lu avg_int_to_read_us=%s max_us=%s", U(r.keysImuOff.n),
         F(r.keysImuOff.avgUs, 0, r.keysImuOff.n > 0).s, F(r.keysImuOff.maxUs, 0, r.keysImuOff.n > 0).s);
  e.line("keys_imu_on: n=%lu avg_int_to_read_us=%s max_us=%s", U(r.keysImuOn.n),
         F(r.keysImuOn.avgUs, 0, r.keysImuOn.n > 0).s, F(r.keysImuOn.maxUs, 0, r.keysImuOn.n > 0).s);
  e.line("keys: downs=%lu ups=%lu queue_drops=%lu fifo_ovf=%lu i2c_err=%lu unknown=%lu max_int_to_read_us=%lu",
         U(r.keyDowns), U(r.keyUps), U(r.keyDrops), U(r.keyFifoOvf), U(r.keyI2cErr), U(r.keyUnknown),
         U(r.keyMaxIntToReadUs));
  e.line("play: preset=%d shakes=%lu last_shake_g=%s", r.preset + 1, U(r.shakes), F(r.lastShakeG, 2).s);
  e.line("mem: heap_free=%lu heap_min=%lu uptime_s=%lu", U(r.heapFree), U(r.heapMin), U(r.uptimeS));
  e.line("===== PG-HWSPIKE TILT END =====");
  return e.lines();
}

int formatSamplerReport(const SamplerReport& r, LineSink sink, void* user) {
  Emitter e(sink, user);
  e.line("===== PG-HWSPIKE SAMPLER BEGIN %s =====", kReportVersion);
  e.line("store: ok=%d why=%s part=0x%06lX+0x%06lX fs=0x%06lX+0x%06lX running=0x%06lX rec=+0x%06lX/0x%06lX "
         "bench=+0x%06lX/0x%06lX bench_ready=%d",
         r.storeOk ? 1 : 0, (r.storeWhy && r.storeWhy[0]) ? r.storeWhy : "-", U(r.partAddr), U(r.partSize),
         U(r.fsAddr), U(r.fsSize), U(r.runningAddr), U(r.recRegionOff), U(r.recRegionSize), U(r.benchRegionOff),
         U(r.benchRegionSize), r.benchReady ? 1 : 0);
  if (r.regsValid) {
    e.line("codec: r01=%02X r09=%02X r0A=%02X r0D=%02X r0E=%02X r14=%02X r15=%02X r16=%02X r17=%02X r1C=%02X",
           r.regs[0], r.regs[1], r.regs[2], r.regs[3], r.regs[4], r.regs[5], r.regs[6], r.regs[7], r.regs[8],
           r.regs[9]);
  } else {
    e.line("codec: na");
  }
  e.line("input: rate=%lu channel=%s gain=%d (levels in dBFS, sil_rms with DC removed, dc in LSB)", U(r.rate),
         r.channel ? "R" : "L", r.gain + 1);
  for (int c = 0; c < 2; ++c) {
    e.line("live_%s: rms_dbfs=%s peak_dbfs=%s dc_lsb=%s stuck=%d", c ? "R" : "L", F(r.liveRmsDb[c], 1).s,
           F(r.livePeakDb[c], 1).s, F(r.liveDc[c], 1).s, r.liveStuck[c] ? 1 : 0);
  }
  for (int g = 0; g < kNumGainRows; ++g) {
    const GainRow& row = r.gains[g];
    e.line("gain%d: pga_db=%d r14=%02X r16=%02X sil_rms=%s sil_pk=%s sil_dc=%s sil_stuck=%d rec_pk=%s "
           "rec_rms=%s rec_dc=%s rec_clips=%lu rec_s=%s",
           g + 1, row.pgaDb, row.reg14, row.reg16, F(row.silAcRmsDb, 1, row.silValid).s,
           F(row.silPeakDb, 1, row.silValid).s, F(row.silDcLsb, 1, row.silValid).s, row.silStuck ? 1 : 0,
           F(row.recPeakDb, 1, row.recValid).s, F(row.recRmsDb, 1, row.recValid).s, F(row.recDcLsb, 1, row.recValid).s,
           U(row.recClips), F(row.recSecs, 2, row.recValid).s);
  }
  e.line("rec: valid=%d samples=%lu secs=%s chunk=%lu writes=%lu write_avg_us=%s write_max_us=%s erase_ms=%s",
         r.recValid ? 1 : 0, U(r.recSamples), F(r.recSecs, 2).s, U(r.recChunk), U(r.recWrites),
         F(r.recWriteAvgUs, 0).s, F(r.recWriteMaxUs, 0).s, F(r.recEraseMs, 0).s);
  e.line("rec_drops: i2s_rx_overflows=%lu ring_drop_samples=%lu blocks=%lu max_rx_gap_us=%s block_period_us=%s",
         U(r.recRxOverflows), U(r.recRingDrops), U(r.recBlocks), F(r.recMaxRxGapUs, 0).s,
         F(r.recBlockPeriodUs, 0).s);
  e.line("play: avg_us=%s max_us=%s core0_pct=%s", F(r.playAvgUs, 1).s, F(r.playMaxUs, 1).s, F(r.playCpuPct, 1).s);
  e.line("bench: rows=%d miss_cyc_per_line=%s (src voices poll_kb cyc_per_sample cyc_per_voice_sample "
         "max_block_cyc core_pct)",
         r.benchRows, F(r.missCycPerLine, 0, r.missCycPerLine > 0.0f).s);
  for (int i = 0; i < r.benchRows && i < kMaxBenchRows; ++i) {
    const BenchRow& b = r.bench[i];
    e.line("bench_row: %s %d %d %s %s %lu %s", b.src, b.voices, b.pollKb, F(b.cycPerSample, 1).s,
           F(b.cycPerVoiceSample, 1).s, U(b.maxBlockCyc), F(b.corePct, 2).s);
  }
  e.line("mem: heap_free=%lu heap_min=%lu heap_largest=%lu static_dram=%lu uptime_s=%lu", U(r.heapFree),
         U(r.heapMin), U(r.heapLargest), U(r.staticDram), U(r.uptimeS));
  e.line("===== PG-HWSPIKE SAMPLER END =====");
  return e.lines();
}

}  // namespace hws
