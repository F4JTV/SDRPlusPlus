#!/usr/bin/sh
sudo apt update
sudo apt install build-essential cmake git pkg-config libfftw3-dev \
    libglfw3-dev libvolk-dev libzstd-dev mesa-common-dev libgl-dev \
    librtlsdr-dev libhackrf-dev libairspy-dev libairspyhf-dev \
    libusb-1.0-0-dev libiio-dev libad9361-dev \
    libbladerf-dev liblimesuite-dev libsoapysdr-dev libcodec2-dev \
    portaudio19-dev libfaad-dev libopus-dev libfdk-aac-dev \
    libsndfile1-dev libspeexdsp-dev \
    libopenjp2-7-dev libstb-dev libxkbcommon-dev \
    libxcb-image0-dev  libxcb-util1 libxcb-cursor-dev \
    libxcb-randr0-dev libxcb-keysyms1-dev \
    libglew-dev nlohmann-json3-dev \
    libglib2.0-dev zlib1g-dev libxml2-dev \
    libpulse-dev libsndfile1-dev libfftw3-dev liblapack-dev \
    socat rtl-sdr librtlsdr-dev \
    wget libncurses-dev libncurses6 libcodec2-dev

# clone SDRPlusPlus
cd ~
git clone https://github.com/F4JTV/SDRPlusPlus.git

# clone welle.io 
cd ./SDRPlusPlus/decoder_modules/dab_decoder
git clone --depth 1 https://github.com/F4JTV/welle.io.git

# clone Dream
cd ../drm_decoder
git clone https://github.com/F4JTV/dream.git

# install dsd-fme and mbelib
cd ~
git clone https://github.com/F4JTV/mbelib
cd mbelib
git checkout ambe_tones
mkdir build
cd build
cmake ..
make -j $(nproc)
sudo make install
sudo ldconfig
cd ~
git clone https://github.com/F4JTV/dsd-fme
cd dsd-fme
#git checkout audio_work
mkdir build
cd build
cmake ..
make -j $(nproc)
sudo make install
sudo ldconfig

# install rtl-433
cd ~
git clone https://github.com/F4JTV/rtl_433.git
cd rtl_433 && mkdir -p build && cd build
cmake -DENABLE_RTLSDR=OFF -DENABLE_SOAPYSDR=OFF -DENABLE_OPENSSL=OFF \
      -DBUILD_TESTING=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_C_FLAGS="-fPIC" ..
make r_433 -j$(nproc)

# install map
cd ~/SDRPlusPlus/misc_modules/sdr_map_launcher/sdr_map
python3 -m pip install --user --break-system-packages -r ./requirements.txt
python3 ./manage.py migrate

# install vdl2dump and libacars
cd ~
git clone https://github.com/F4JTV/libacars
cd libacars 
mkdir build 
cd build  
cmake .. 
make  
sudo make install  
sudo ldconfig
cd ../..
git clone https://github.com/F4JTV/dumpvdl2
cd dumpvdl2  
mkdir build
cd build  
cmake ..  
make  
sudo make install

# Compile SDR++ and modules
cd ~/SDRPlusPlus
mkdir build
cd build
cmake .. -DRTL_433_ROOT=/home/$USER/rtl_433
make -j$(nproc)
sudo make install

