# Linux cgroup tools:

### Building
```bash
git clone https://github.com/ViperCraft/cg-tools.git
cd cg-tools
make
```

### show-pagemap utility

This utility examine cgroups memory mapping, active(RSS) and shared parts. Useful to finds memory usage anomaly, especially if you not yet "memory guru" in Linux. Does not change any mappings, works only as read only. Does not change page/disk cache references, only if Linux kernel needs it during directory traversal.

**Usage**
```bash
Usage: show-pagemap [options] <PID/DIR>
options are following:
        -D|--dir:                      treat PID as DIR name and traverse files as page-cache.
        -d|--details:                  show per page details.
        -g|--cgroup:                   show cgroup refs from /proc/kpagecgroup.
        -r|--refs:                     show sharing refs from /proc/kpagecount.
        -n|--names:                    show map name if found.
        -m|--mount:                    override cgroup mount, default is /sys/fs/cgroup/.
```

**Per process mode**
This is default mode, you need to pass PIDs:
```bash
sudo ./show-pagemap -g $(pidof python3) | grep ddb4057f992264502e4351bd56c0eb19e34fb34682cc3c399b2f10c7b9e74968
{22907}/sys/fs/cgroup/memory/docker/ddb4057f992264502e4351bd56c0eb19e34fb34682cc3c399b2f10c7b9e74968 103550     = 404.49 MB
```

**DiskCache back-traverse mode**
This is extended mode, traverse provided directory and examines regular files one by one and look into disk-cache data and collects like in per-process mode, but this time w/o processes.
```bash

```

***Expert mode***
TODO

### Limitations

- No HugePages support, but if you plan to use only DiskCache mode, this will be not necessary.
- Enabling counting references pages shows only total pages with refs more than 1
- Traverse disk-cache take some time, so actual memory layout can be changed during measurements, kept in mind.


