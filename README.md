skull
=====

Fast to start-up, easy to maintain, high productivity server framework.

## How to Build
Use Ubuntu14.04 as an example.

### Install dependencies
```
apt-get install libyaml-dev libpcap0.8-dev libpcap0.8 libprotobuf-c0 libprotobuf-c0-dev

git clone git@github.com:finaldie/skull.git
cd skull
git submodule update --init --recursive
make dep
```

### Build Release Version
```
make -j4
```

### Build Debug Version
```
make debug=true -j4
```

## How to Run
```
skull -c config
```
