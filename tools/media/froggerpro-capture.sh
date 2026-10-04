#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Configure FroggerPro RDI0 and collect evidence for camera bring-up.

set -eu

mode=${1:-imx355-bars}
if [ "$#" -gt 2 ]; then
	echo "Usage: $0 [imx355-bars|imx355|tpg] [output-directory]" >&2
	exit 2
fi
case "$mode" in
	imx355|imx355-bars|tpg) ;;
	*) echo "Usage: $0 [imx355-bars|imx355|tpg] [output-directory]" >&2; exit 2 ;;
esac

for tool in media-ctl v4l2-ctl timeout; do
	command -v "$tool" >/dev/null || {
		echo "Missing command: $tool" >&2
		exit 1
	}
done

if [ "$#" -gt 1 ]; then
	mkdir "$2"
	result_dir=$(cd "$2" && pwd)
else
	result_dir=$(mktemp -d /tmp/froggerpro-capture.XXXXXX)
fi

echo "Capture diagnostics: $result_dir"
exec >"$result_dir/commands.log" 2>&1

media_device=${MEDIA_DEVICE:-}
finish()
{
	status=$?
	trap - EXIT
	set +e
	cat /proc/interrupts >"$result_dir/interrupts-after.txt"
	dmesg >"$result_dir/dmesg.txt" 2>&1
	if [ -n "$media_device" ]; then
		media-ctl -d "$media_device" -p >"$result_dir/topology-after.txt" 2>&1
	fi
	echo "Exit status: $status"
	exit "$status"
}
trap finish EXIT

run()
{
	printf '+'
	printf ' %s' "$@"
	printf '\n'
	"$@"
}

if [ -z "$media_device" ]; then
	for candidate in /dev/media*; do
		if media-ctl -d "$candidate" -e msm_vfe0_rdi0 >/dev/null 2>&1; then
			media_device=$candidate
			break
		fi
	done
fi
if [ -z "$media_device" ]; then
	echo 'No CAMSS media device found. Check CAMSS/IMX355 probe and loaded modules.'
	exit 1
fi

run uname -a
media-ctl -d "$media_device" -p >"$result_dir/topology-before.txt"
cat /proc/interrupts >"$result_dir/interrupts-before.txt"
video_device=$(media-ctl -d "$media_device" -e msm_vfe0_video0)

if [ "$mode" = tpg ]; then
	source_entity=msm_tpg0
	source_device=$(media-ctl -d "$media_device" -e "$source_entity")
	bus_code=SBGGR8_1X8
	size=640x480
	pixel_format=BA81
	width=640
	height=480
else
	source_entity=$(sed -n 's/.*entity [0-9]*: \(imx355 [^ ]*\) (.*/\1/p' \
		"$result_dir/topology-before.txt")
	if [ -z "$source_entity" ]; then
		echo 'IMX355 is absent from the media topology. Check sensor probe.'
		exit 1
	fi
	source_device=$(media-ctl -d "$media_device" -e "$source_entity")
	bus_code=SRGGB10_1X10
	size=3280x2464
	pixel_format=pRAA
	width=3280
	height=2464
fi

# Reset mutable links; the sensor-to-CSIPHY link is immutable and enabled.
run media-ctl -d "$media_device" -r
if [ "$mode" = tpg ]; then
	run media-ctl -d "$media_device" -l '"msm_tpg0":0 -> "msm_csid0":0 [1]'
	run v4l2-ctl -d "$source_device" --set-ctrl=test_pattern=9
else
	run media-ctl -d "$media_device" -l '"msm_csiphy0":1 -> "msm_csid0":0 [1]'
	pattern=0
	[ "$mode" != imx355-bars ] || pattern=2
	run v4l2-ctl -d "$source_device" \
		--set-ctrl="horizontal_flip=0,vertical_flip=0,test_pattern=$pattern"
fi
run media-ctl -d "$media_device" -l '"msm_csid0":1 -> "msm_vfe0_rdi0":0 [1]'
run media-ctl -d "$media_device" -V "\"$source_entity\":0 [fmt:$bus_code/$size]"
if [ "$mode" != tpg ]; then
	run media-ctl -d "$media_device" -V "\"msm_csiphy0\":0 [fmt:$bus_code/$size]"
fi
for entity in msm_csid0 msm_vfe0_rdi0; do
	for pad in 0 1; do
		run media-ctl -d "$media_device" -V "\"$entity\":$pad [fmt:$bus_code/$size]"
	done
done

media-ctl -d "$media_device" -p >"$result_dir/topology-configured.txt"
run v4l2-ctl -d "$source_device" --all
run v4l2-ctl -d "$video_device" \
	--set-fmt-video="width=$width,height=$height,pixelformat=$pixel_format"
run v4l2-ctl -d "$video_device" --get-fmt-video
run timeout 20s v4l2-ctl -d "$video_device" --verbose \
	--stream-mmap=4 --stream-count=10 --stream-to="$result_dir/frames.raw"

if [ ! -s "$result_dir/frames.raw" ]; then
	echo 'Capture returned no frame data.'
	exit 1
fi
run wc -c "$result_dir/frames.raw"
echo 'Captured raw frames; inspect their content using the negotiated format and stride.'
