# mi-xdrd — build xdrd.c on debian:trixie-slim with hot-plug USB support.

FROM debian:trixie-slim AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential libssl-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY xdrd.c xdr-protocol.h Makefile ./
RUN make

FROM debian:trixie-slim

# udev + util-linux: needed by entry.sh to remount /dev as devtmpfs and run
# udevd, so hot-plugged USB devices are visible inside the container.
# gosu: xdrd.c refuses to run as uid=0, so entry.sh runs as root for the
# mount/udevd setup and CMD drops to the xdrd user before exec.
RUN apt-get update && \
    apt-get install -y --no-install-recommends libssl3 udev util-linux gosu && \
    rm -rf /var/lib/apt/lists/* && \
    useradd -r -u 1000 -s /usr/sbin/nologin xdrd

COPY --from=builder /src/xdrd /usr/local/bin/xdrd
COPY entry.sh /usr/local/bin/entry.sh
RUN chmod +x /usr/local/bin/entry.sh

# udev rules create /dev/ttyTEF<N> + /dev/snd/tef-N-* symlinks based on
# physical hub port, with MODE=0666 so the non-root xdrd user can open them.
COPY 99-tef-tuners.rules /etc/udev/rules.d/99-tef-tuners.rules

# UDEV=on triggers the devtmpfs remount + udevd startup in entry.sh.
# Requires `privileged: true` in docker-compose.
ENV UDEV=on

ENTRYPOINT ["/usr/local/bin/entry.sh"]
CMD ["/bin/sh", "-c", "exec gosu xdrd xdrd -s \"$XDRD_SERIAL_PORT\" -t \"$XDRD_TCP_PORT\" -p \"$XDRD_PASSWORD\" -g"]
