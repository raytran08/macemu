#!/bin/sh
# runbisect.sh - start X, walk wedgebisect through its stages, and report
# the first one after which the server stops answering.
#
# Runs on the target.  Each stage is a separate process that cleans up
# after itself, so a stage that does not wedge leaves the server fit for
# the next; only the first failure matters.
#
# X takes about a minute to come up on this machine, so the wait is
# generous rather than optimistic.
PATH=/sbin:/usr/sbin:/bin:/usr/bin:/usr/X11R7/bin:/usr/pkg/bin:/usr/local/bin
export PATH
DISPLAY=:1
export DISPLAY
XAUTHORITY=/home/maxim/.Xauthority
export XAUTHORITY

cleanup() {
	pkill -9 wedgebisect 2>/dev/null
	pkill xinit 2>/dev/null
	sleep 3
	pkill -9 Xwscons 2>/dev/null
	sleep 2
	wsreset /dev/ttyE0 >/dev/null 2>&1
}

# Is the server answering anyone at all?  Bounded, because a wedged
# server makes every client hang -- which is the whole phenomenon.
x_alive() {
	rm -f /tmp/xalive.out
	(
		xdpyinfo >/tmp/xalive.out 2>&1 &
		p=$!
		sleep 10
		kill -9 $p 2>/dev/null
	)
	[ -s /tmp/xalive.out ]
}

echo "=== starting X ==="
pkill -9 Xwscons 2>/dev/null
sleep 1
su - maxim -c 'nohup /usr/local/bin/startxws >/tmp/xstart.log 2>&1 &'

i=0
while [ $i -lt 24 ]; do
	sleep 5
	i=$((i + 1))
	if x_alive; then
		echo "  X up after $((i * 5))s"
		break
	fi
done
if ! x_alive; then
	echo "  X NEVER CAME UP -- see /tmp/xstart.log"
	tail -5 /tmp/xstart.log 2>/dev/null
	cleanup
	exit 1
fi

for s in 1 2 3 4 5 6; do
	echo "=== stage $s ==="
	(
		/home/maxim/basilisk/wedgebisect $s 2>&1 &
		p=$!
		sleep 25
		kill -9 $p 2>/dev/null
	)
	if x_alive; then
		echo "  -> X still answering"
	else
		echo "  -> X WEDGED. Stage $s is the culprit."
		cleanup
		exit 0
	fi
done

echo "=== all stages passed; the wedge is elsewhere in the driver ==="
cleanup
exit 0
