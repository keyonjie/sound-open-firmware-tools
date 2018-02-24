# Low Latency Pipeline and PCM
#
# Pipeline Endpoints for connection are :-
#
#  host PCM_C <--B5-- volume(0C) <--B4-- source DAI0

# Include topology builder
include(`local.m4')

#
# Controls
#

SectionControlMixer.STR(PCM PCM_ID Capture Volume) {

	# control belongs to this index group
	index STR(PIPELINE_ID)

	# Channel register and shift for Front Left/Right
	channel."FL" {
		reg "0"
		shift "0"
	}
	channel."FR" {
		reg "0"
		shift "1"
	}

	# control uses bespoke driver get/put/info ID 0
	ops."ctl" {
		info "volsw"
		get "256"
		put "256"
	}

	# TLV 40 steps from -90dB to +20dB for 3dB
	max "40"
	invert "false"
	tlv "vtlv_m90s3"
}

#
# Components and Buffers
#

# Host "Low Latency Capture" PCM uses pipeline DMAC and channel
# with 0 sink and 2 source periods
W_PCM_CAPTURE(Low Latency Capture, PIPELINE_DMAC, PIPELINE_DMAC_CHAN, 0, 2, 0)

# "Capture Volume" has 2 sink and source periods for host and DAI ping-pong
W_PGA(0, PCM PCM_ID Capture Volume, PIPELINE_FORMAT, 2, 2, 0)

# Capture Buffers
W_BUFFER(0, COMP_BUFFER_SIZE(2,
	COMP_SAMPLE_SIZE(PIPELINE_FORMAT), PIPELINE_CHANNELS, SCHEDULE_FRAMES))
W_BUFFER_DMA(1, COMP_BUFFER_SIZE(2,
	COMP_SAMPLE_SIZE(PIPELINE_FORMAT), PIPELINE_CHANNELS, SCHEDULE_FRAMES))

#
# Pipeline Graph
#
#  host PCM <--B1-- volume <--B0-- source DAI0

SectionGraph."pipe-ll-capture-PIPELINE_ID" {
	index STR(PIPELINE_ID)

	lines [
		dapm(Low Latency Capture PCM_ID, N_PCMC)
		dapm(N_PCMC, N_BUFFER(1))
		dapm(N_BUFFER(1), N_PGA(0))
		dapm(N_PGA(0), N_BUFFER(0))
	]
}

#
# Pipeline Source and Sinks
#
indir(`define', concat(`PIPELINE_SINK_', PIPELINE_ID), N_BUFFER(0))
indir(`define', concat(`PIPELINE_PCM_', PIPELINE_ID), Low Latency Capture PCM_ID)

#
# PCM Configuration
#

SectionPCMCapabilities.STR(Low Latency Capture PCM_ID) {

	formats "S32_LE,S24_LE,S16_LE"
	rate_min "48000"
	rate_max "48000"
	channels_min "2"
	channels_max "4"
	periods_min "2"
	periods_max "4"
	period_size_min	"192"
	period_size_max	"16384"
	buffer_size_min	"65536"
	buffer_size_max	"65536"
}

