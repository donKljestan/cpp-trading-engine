# Deployment runbook

Target: a single Ubuntu 24.04 x86_64 instance in AWS `ap-northeast-1`, the region that
hosts the Binance matching engine. Nothing here needs an API key; the recorder consumes
public market data only.

## First time

```bash
git clone <repo> ~/low_latency_trading
cd ~/low_latency_trading
./deploy/provision.sh
./build.sh release
```

Confirm the location was worth it before going further:

```bash
./build/release/lat_probe 30
```

Expect a p50 round trip in the low single-digit milliseconds and a verdict of
`colocated` or `same region`. If it reports anything above 50 ms, the instance is not
where it should be — check the region before debugging anything else.

## Recording as a service

```bash
sudo mkdir -p /etc/llt
printf 'LLT_SYMBOLS=TACUSDT,CFGUSDT,CVXUSDT,DATAIPUSDT,HANAUSDT\n' \
  | sudo tee /etc/llt/record.env
sudo chmod 600 /etc/llt/record.env

sudo cp deploy/llt-record.service /etc/systemd/system/
sudo cp deploy/llt-compress.service deploy/llt-compress.timer /etc/systemd/system/
sudo systemctl daemon-reload

sudo systemctl enable --now llt-record.service
sudo systemctl enable --now llt-compress.timer
```

## Checking on it

```bash
systemctl status llt-record
journalctl -u llt-record -n 50 --no-pager
ls -lh ~/data | tail
df -h /
```

Reconnects are normal and appear in the journal; the venue drops long-lived
connections. What matters is that files keep appearing and the disk is not filling
faster than projected.

To inspect what was actually captured:

```bash
./build/release/md_replay ~/data/md-*.mdlog
```

## Updating

```bash
cd ~/low_latency_trading
git pull
./build.sh release
sudo systemctl restart llt-record
```

## Known rough edge

`md_replay` reads uncompressed captures only. Decompress first:

```bash
gunzip -k ~/data/md-20261028-120000.mdlog.gz
```

Transparent `.gz` support would mean shelling out to `gzip` with a filename from the
command line, which is a shell-injection surface. Not worth adding to a program that
will later hold API credentials, for the sake of saving one command.

## Cost

```
t3.small on-demand   ~$15.20 / month
60 GB gp3             ~$4.80 / month
Elastic IP                 $0 while attached
```

`systemctl stop llt-record` pauses recording; stopping the instance pauses compute
charges but not storage. An unattached Elastic IP is billed, so release it if the
instance is terminated.
