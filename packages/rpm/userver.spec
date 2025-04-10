%define __boost_suffix 176
%global debug_package %{nil}  # remove when so libraries will be added to package

Name:    userver-devel
Version: %{_version}
Release: %{_release}%{?dist}
Summary: userver framework
Group:   Development/Libraries/C and C++
License: Apache License 2.0
URL:     https://github.com/userver-framework/userver
Source0: %{_source}
BuildRequires: cmake
BuildRequires: git-clang-format
BuildRequires: python36 python3-libs
BuildRequires: gcc-c++
BuildRequires: cctz-devel gtest-devel spdlog-devel
Requires: cctz gtest spdlog
BuildRequires: libnghttp2-devel libidn2-devel brotli-devel c-ares-devel
Requires: libnghttp2 libidn2 brotli c-ares
BuildRequires: libcurl-devel libbson-devel
Requires: libcurl libbson
BuildRequires: mongo-c-driver-libs mongo-c-driver
Requires: mongo-c-driver-libs mongo-c-driver
BuildRequires: fmt-devel
Requires: fmt
BuildRequires: libssh-devel
Requires: libssh
BuildRequires: hiredis-devel
Requires: hiredis
BuildRequires: rapidjson-devel
BuildRequires: moodycamel-concurrentqueue-devel
BuildRequires: compiler-rt
BuildRequires: boost%{__boost_suffix}-devel >= 1.67.0
BuildRequires: amqp-cpp-devel
BuildRequires: clickhouse-cpp-devel
BuildRequires: libatomic
Requires: libatomic
BuildRequires: cryptopp-devel
Requires: cryptopp
BuildRequires: libev-devel
Requires: libev
BuildRequires: re2-devel
Requires: re2
BuildRequires: libiconv-devel
Requires: libiconv
BuildRequires: python3-jinja2

BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{_version}-%{release}-XXXXXX)

%description

Open source asynchronous framework with a rich set of abstractions for fast and comfortable creation of C++ microservices, services and utilities.

%package -n %{name}-devel
Summary: userver framework
Group:   Development/Libraries/C and C++
Requires: %{name} = %{version}
Requires: cctz-devel gtest-devel spdlog-devel
Requires: libnghttp2-devel libidn2-devel brotli-devel c-ares-devel
Requires: libcurl-devel libbson-devel
Requires: mongo-c-driver-libs mongo-c-driver
Requires: fmt-devel
Requires: libssh-devel
Requires: hiredis-devel
Requires: clickhouse-cpp
Requires: amqp-cpp
#Requires: boost-stacktrace >= 1.66.0

%description -n %{name}-devel

Open source asynchronous framework with a rich set of abstractions for fast and comfortable creation of C++ microservices, services and utilities.

%prep
%setup -q -n userver-%{_version}

%build
rm -rf build
mkdir build
pushd build
export PYTHONPATH=$PYTHONPATH:/usr/local/lib/python3.6/site-packages

# -DUSERVER_LTO:BOOL=OFF as workaround for dwz crash
cmake -DUSERVER_FEATURE_PATCH_LIBPQ=0 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DUSERVER_PYTHON_PATH=/usr/bin/python3 \
  -DCMAKE_INSTALL_PREFIX:PATH=/usr \
  -DCMAKE_INSTALL_LIBDIR=lib64 \
  -DUSERVER_DOWNLOAD_PACKAGES:BOOL=OFF \
  -DUSERVER_LTO:BOOL=OFF \
  -DUSERVER_GEN_GDB_DEBUGINFO:BOOL=ON \
  -DUSERVER_INSTALL:BOOL=ON \
  -DCMAKE_CXX_COMPILER=/opt/rh/gcc-toolset-10/root/usr/bin/g++ \
  -DCMAKE_C_COMPILER=/opt/rh/gcc-toolset-10/root/usr/bin/gcc \
  -DUSERVER_FEATURE_STACKTRACE:BOOL=OFF \
  -DUSERVER_FEATURE_GRPC:BOOL=ON \
  -DUSERVER_FEATURE_TESTSUITE:BOOL=OFF \
  ..

cmake --build . -j 8
popd

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
echo "Install to %{buildroot}"

pushd build
cmake --install . --prefix %{buildroot}/usr/

# move third party installations into lib64
#cp -r %{buildroot}/usr/lib %{buildroot}/usr/lib64/
#rm -rf %{buildroot}/usr/lib

# TODO cmake config install
#mv %{buildroot}/usr/cmake %{buildroot}/usr/lib64/
#rm -rf %{buildroot}/usr/cmake
popd

%clean
rm -rf %{buildroot}

#%files -n %{name}
#%{_libdir}/lib*.a
#%{_libdir}/userver/

#%files -n % {name}-devel
%files -n %{name}
%{_includedir}/userver/
%{_bindir}/*
%{_libdir}/*
