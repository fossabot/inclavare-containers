%define centos_base_release 1
%define _debugsource_template %{nil}

%global PROJECT inclavare-containers
%global SHIM_BIN_DIR /usr/local/bin
%global SHIM_CONFIG_DIR /etc/inclavare-containers
# to skip no build id error
%undefine _missing_build_ids_terminate_build

Name: shim-rune
Version: 0.6.3
Release: %{centos_base_release}%{?dist}
Summary: shim for Inclavare Containers(runE)
Group: Development/Tools
License: Apache License 2.0
URL: https://github.com/alibaba/%{PROJECT}
Source0: https://github.com/alibaba/%{PROJECT}/archive/v%{version}.tar.gz

ExclusiveArch: x86_64

%description
containerd-shim-rune-v2 is a shim for Inclavare Containers(runE).

%prep
%setup -q -n %{PROJECT}-%{version}

%build
# we can't download go 1.13 through 'yum install' in centos, so that we check the go version in the '%build' section rather than in the 'BuildRequires' section.
if ! [ -x "$(command -v go)" ]; then
  echo 'Error: go is not installed. Please install Go 1.13 and above'
  exit 1
fi

NEED_GO_VERSION=13
CURRENT_GO_VERSION=$(go version | awk '{print $3}' | sed 's/go//g' | sed 's/\./ /g' | awk '{print $2}')
if [ $CURRENT_GO_VERSION -lt $NEED_GO_VERSION  ]; then
  echo 'Error: go version is less than 1.13.0. Please install Go 1.13 and above'
  exit 1
fi

export GOPATH=${RPM_BUILD_DIR}/%{PROJECT}-%{version}
export GOPROXY="https://mirrors.aliyun.com/goproxy,direct"
cd shim
GOOS=linux make binaries

%install
install -d -p %{buildroot}%{SHIM_BIN_DIR}
install -p -m 755 shim/bin/containerd-shim-rune-v2 %{buildroot}%{SHIM_BIN_DIR}

install -d -p %{buildroot}%{_defaultlicensedir}/%{name}
install -p -m 644 shim/LICENSE %{buildroot}%{_defaultlicensedir}/%{name}

%post
mkdir -p %{SHIM_CONFIG_DIR}
cat << EOF > %{SHIM_CONFIG_DIR}/config.toml
log_level = "info" # "debug" "info" "warn" "error"
sgx_tool_sign = "/opt/intel/sgxsdk/bin/x64/sgx_sign"

[containerd]
    socket = "/run/containerd/containerd.sock"
# The epm section is optional.
# If the epm serivce is deployed, you can configure a appropriate unix socket address in "epm.socket" field,
# otherwise just delete the epm section.
[epm]
    socket = "/var/run/epm/epm.sock"
[enclave_runtime]
    # The signature_method represents the signature method for enclave.
    # It can be "server" or "client", the default value is "server"
    signature_method = "server"
    [enclave_runtime.occlum]
        enclave_runtime_path = "/opt/occlum/build/lib/libocclum-pal.so"
        enclave_libos_path = "/opt/occlum/build/lib/libocclum-libos.so"
    [enclave_runtime.graphene]
EOF

%postun
rm -f %{SHIM_CONFIG_DIR}/config.toml

%files
%{_defaultlicensedir}/%{name}/LICENSE
%{SHIM_BIN_DIR}/containerd-shim-rune-v2

%changelog
* Mon Aug 30 2021 Shirong Hao <shirong@linux.alibaba.com> - 0.6.3
- Update to version 0.6.3

* Wed Jun 30 2021 Zhiguang Jia <Zhiguang.Jia@linux.alibaba.com> - 0.6.2
- Update to version 0.6.2

* Mon May 24 2021 Liang Yang <Liang3.Yang@intel.com> - 0.6.1
- Update to version 0.6.1

* Mon Feb 8 2021 Zhiguang Jia <Zhiguang.Jia@linux.alibaba.com> - 0.6.0
- Update to version 0.6.0

* Wed Dec 30 2020 Zhiguang Jia <Zhiguang.Jia@linux.alibaba.com> - 0.5.2
- Update to version 0.5.2

* Mon Nov 30 2020 Zhiguang Jia <Zhiguang.Jia@linux.alibaba.com> - 0.5.1
- Update to version 0.5.1

* Tue Oct 27 2020 Zhiguang Jia <Zhiguang.Jia@linux.alibaba.com> - 0.5.0
- Update to version 0.5.0

* Sun Sep 27 2020 Zhiguang Jia <Zhiguang.Jia@linux.alibaba.com> - 0.4.1
- Update to version 0.4.1

* Sun Aug 30 2020 Shirong hao <shirong@linux.alibaba.com> - 0.4.0
- Update to version 0.4.0

* Tue Jul 28 2020 shirong <shirong@linux.alibaba.com> - 0.3.0
- Update to version 0.3.0

* Fri Jul 10 2020 Zhiguang Jia <Zhiguang.Jia@linux.alibaba.com> - 0.2.0
- Package init.
