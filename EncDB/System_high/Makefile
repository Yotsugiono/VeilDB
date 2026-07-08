SGXSSL_LIB_PATH ?= /opt/intel/sgxssl/lib64
SGXSSL_INCLUDE_PATH ?= /opt/intel/sgxssl/include


######## SGX SDK Settings ########

SGX_SDK ?= /opt/intel/sgxsdk
SGX_MODE ?= SIM
SGX_ARCH ?= x64

ifeq ($(shell getconf LONG_BIT), 32)
	SGX_ARCH := x86
else ifeq ($(findstring -m32, $(CXXFLAGS)), -m32)
	SGX_ARCH := x86
endif

ifeq ($(SGX_ARCH), x86)
	SGX_COMMON_CFLAGS := -m32
	SGX_LIBRARY_PATH := $(SGX_SDK)/lib
	SGX_ENCLAVE_SIGNER := $(SGX_SDK)/bin/x86/sgx_sign
	SGX_EDGER8R := $(SGX_SDK)/bin/x86/sgx_edger8r
else
	SGX_COMMON_CFLAGS := -m64
	SGX_LIBRARY_PATH := $(SGX_SDK)/lib64
	SGX_ENCLAVE_SIGNER := $(SGX_SDK)/bin/x64/sgx_sign
	SGX_EDGER8R := $(SGX_SDK)/bin/x64/sgx_edger8r
endif

ifeq ($(SGX_DEBUG), 1)
ifeq ($(SGX_PRERELEASE), 1)
$(error Cannot set SGX_DEBUG and SGX_PRERELEASE at the same time!!)
endif
endif

ifeq ($(SGX_DEBUG), 1)
        SGX_COMMON_CFLAGS += -O0 -g
else
        SGX_COMMON_CFLAGS += -O2
endif

######## App Settings ########

ifneq ($(SGX_MODE), HW)
	Urts_Library_Name := sgx_urts_sim
else
	Urts_Library_Name := sgx_urts
endif

Client_Cpp_Files := \
    CryptoTestingApp/client_main.cpp \
    CryptoTestingApp/Client.cpp \
    CryptoTestingApp/net.cpp \
    CryptoTestingApp/RPC.cpp \
    CryptoTestingApp/Utils.cpp
App_Include_Paths := -IInclude -IApp -I$(SGX_SDK)/include -I/usr/include -I/usr/include/openssl

Server_Cpp_Files := \
    CryptoTestingApp/server_main.cpp \
    CryptoTestingApp/Server.cpp \
    CryptoTestingApp/net.cpp \
    CryptoTestingApp/RPC.cpp \
    CryptoTestingApp/Utils.cpp \
	CryptoTestingApp/EdbDiskStore.cpp \
	CryptoTestingApp/EncDBManager.cpp

Benchmark_Cpp_Files := \
    CryptoTestingApp/benchmark_main.cpp \
    CryptoTestingApp/Server.cpp \
    CryptoTestingApp/net.cpp \
    CryptoTestingApp/RPC.cpp \
    CryptoTestingApp/Utils.cpp \
	CryptoTestingApp/EdbDiskStore.cpp \
	CryptoTestingApp/EncDBManager.cpp

App_C_Flags := $(SGX_COMMON_CFLAGS) -fPIC -Wno-attributes $(App_Include_Paths)

# Three configuration modes - Debug, prerelease, release
#   Debug - Macro DEBUG enabled.
#   Prerelease - Macro NDEBUG and EDEBUG enabled.
#   Release - Macro NDEBUG enabled.
ifeq ($(SGX_DEBUG), 1)
        App_C_Flags += -DDEBUG -UNDEBUG -UEDEBUG
else ifeq ($(SGX_PRERELEASE), 1)
        App_C_Flags += -DNDEBUG -DEDEBUG -UDEBUG
else
        App_C_Flags += -DNDEBUG -UEDEBUG -UDEBUG
endif

App_Cpp_Flags := $(App_C_Flags) -std=c++17
App_Link_Flags := $(SGX_COMMON_CFLAGS) -L$(SGX_LIBRARY_PATH) -l$(Urts_Library_Name) -lpthread -lpcap -L$(SGXSSL_LIB_PATH) -lsgx_usgxssl -lssl -lcrypto

ifneq ($(SGX_MODE), HW)
	App_Link_Flags += -lsgx_uae_service_sim
else
	App_Link_Flags += -lsgx_uae_service
endif

Client_Cpp_Objects := $(Client_Cpp_Files:.cpp=.o)
Server_Cpp_Objects := $(Server_Cpp_Files:.cpp=.o)
Benchmark_Cpp_Objects := $(Benchmark_Cpp_Files:.cpp=.o)
# client/server/benchmark 共享部分 .o（如 RPC.cpp.o），clean 时须全部删除
App_All_Cpp_Objects := $(sort $(Client_Cpp_Objects) $(Server_Cpp_Objects) $(Benchmark_Cpp_Objects))
App_Generated_Files := CryptoTestingApp/CryptoEnclave_u.c CryptoTestingApp/CryptoEnclave_u.o
Enclave_Generated_Files := CryptoEnclave/CryptoEnclave_t.c CryptoEnclave/CryptoEnclave_t.o
Client_App_Name := encdb_client
Server_App_Name := encdb_server
Benchmark_App_Name := encdb_benchmark

######## Enclave Settings ########
ifneq ($(SGX_MODE), HW)
	Trts_Library_Name := sgx_trts_sim
	Service_Library_Name := sgx_tservice_sim
else
	Trts_Library_Name := sgx_trts
	Service_Library_Name := sgx_tservice
endif
Crypto_Library_Name := sgx_tcrypto
CryptoEnclave_Cpp_Files := $(wildcard CryptoEnclave/*.cpp)
CryptoEnclave_Include_Paths := -IInclude -ICryptoEnclave -I$(SGX_SDK)/include -I$(SGX_SDK)/include/libcxx -I$(SGX_SDK)/include/tlibc -I$(SGXSSL_INCLUDE_PATH) -include "tsgxsslio.h"

CryptoEnclave_C_Flags := $(SGX_COMMON_CFLAGS) -nostdinc -fvisibility=hidden -fpie -fstack-protector $(CryptoEnclave_Include_Paths)
CryptoEnclave_Cpp_Flags := $(CryptoEnclave_C_Flags) -std=c++17 -nostdinc++

CryptoEnclave_Link_Flags := $(SGX_COMMON_CFLAGS) -Wl,--no-undefined -nostdlib -nodefaultlibs -nostartfiles -L$(SGX_LIBRARY_PATH) -L$(SGXSSL_LIB_PATH) \
	-Wl,--whole-archive -l$(Trts_Library_Name) -Wl,--no-whole-archive \
	-Wl,--start-group -lsgx_tstdc -lsgx_tcxx -l$(Crypto_Library_Name) -l$(Service_Library_Name) \
    -Wl,--whole-archive -lsgx_tsgxssl \
    -Wl,--no-whole-archive -lsgx_tsgxssl_crypto \
    -Wl,--end-group \
	-Wl,-Bstatic -Wl,-Bsymbolic -Wl,--no-undefined \
	-Wl,-pie,-eenclave_entry -Wl,--export-dynamic  \
	-Wl,--defsym,__ImageBase=0 \
	-Wl,--version-script=CryptoEnclave/CryptoEnclave.lds
CryptoEnclave_Cpp_Objects := $(CryptoEnclave_Cpp_Files:.cpp=.o)
CryptoEnclave_Name := CryptoEnclave.so
Signed_CryptoEnclave_Name := CryptoEnclave.signed.so
CryptoEnclave_Config_File := CryptoEnclave/CryptoEnclave.config.xml
ifeq ($(SGX_MODE), HW)
ifneq ($(SGX_DEBUG), 1)
ifneq ($(SGX_PRERELEASE), 1)
Build_Mode = HW_RELEASE
endif
endif
endif

.PHONY: all run clean rebuild

ifeq ($(Build_Mode), HW_RELEASE)
all: $(Client_App_Name) $(Server_App_Name) $(Benchmark_App_Name) $(CryptoEnclave_Name)
	@echo "The project has been built in release hardware mode."
	@echo "Please sign the $(CryptoEnclave_Name) first with your signing key before you run the $(Server_App_Name) to launch and access the enclave."
	@echo "To sign the enclave use the command:"
	@echo "   $(SGX_ENCLAVE_SIGNER) sign -key <your key> -enclave $(CryptoEnclave_Name) -out <$(Signed_CryptoEnclave_Name)> -config $(CryptoEnclave_Config_File)"
	@echo "You can also sign the enclave using an external signing tool. See User's Guide for more details."
	@echo "To build the project in simulation mode set SGX_MODE=SIM. To build the project in prerelease mode set SGX_PRERELEASE=1 and SGX_MODE=HW."
else
all: $(Client_App_Name) $(Server_App_Name) $(Benchmark_App_Name) $(Signed_CryptoEnclave_Name)

endif

run: all
ifneq ($(Build_Mode), HW_RELEASE)
	@$(CURDIR)/$(Server_App_Name)
	@echo "RUN  =>  $(Server_App_Name) [$(SGX_MODE)|$(SGX_ARCH), OK]"
endif

######## CryptoTestingApp Objects ########
CryptoTestingApp/CryptoEnclave_u.c: $(SGX_EDGER8R) CryptoEnclave/CryptoEnclave.edl
	@cd CryptoTestingApp && $(SGX_EDGER8R) --untrusted ../CryptoEnclave/CryptoEnclave.edl --search-path ../CryptoEnclave --search-path $(SGX_SDK)/include
	@echo "GEN  =>  $@"
CryptoTestingApp/CryptoEnclave_u.o: CryptoTestingApp/CryptoEnclave_u.c
	@$(CC) $(App_C_Flags) -c $< -o $@
	@echo "CC   <=  $<"
CryptoTestingApp/%.o: CryptoTestingApp/%.cpp
	@$(CXX) $(App_Cpp_Flags) -c $< -o $@
	@echo "CXX  <=  $<"
$(Client_App_Name): $(Client_Cpp_Objects)
	@$(CXX) $^ -o $@ $(App_Link_Flags)
	@echo "LINK =>  $@"
$(Server_App_Name): CryptoTestingApp/CryptoEnclave_u.o $(Server_Cpp_Objects)
	@$(CXX) $^ -o $@ $(App_Link_Flags)
	@echo "LINK =>  $@"
$(Benchmark_App_Name): CryptoTestingApp/CryptoEnclave_u.o $(Benchmark_Cpp_Objects)
	@$(CXX) $^ -o $@ $(App_Link_Flags)
	@echo "LINK =>  $@"

######## CryptoEnclave Objects ########
CryptoEnclave/CryptoEnclave_t.c: $(SGX_EDGER8R) CryptoEnclave/CryptoEnclave.edl
	@cd CryptoEnclave && $(SGX_EDGER8R) --trusted ../CryptoEnclave/CryptoEnclave.edl --search-path ../CryptoEnclave --search-path $(SGX_SDK)/include
	@echo "GEN  =>  $@"
CryptoEnclave/CryptoEnclave_t.o: CryptoEnclave/CryptoEnclave_t.c
	@$(CC) $(CryptoEnclave_C_Flags) -c $< -o $@
	@echo "CC   <=  $<"
CryptoEnclave/%.o: CryptoEnclave/%.cpp
	@$(CXX) $(CryptoEnclave_Cpp_Flags) -c $< -o $@
	@echo "CXX  <=  $<"
$(CryptoEnclave_Name): CryptoEnclave/CryptoEnclave_t.o $(CryptoEnclave_Cpp_Objects)
	@$(CXX) $^ -o $@ $(CryptoEnclave_Link_Flags)
	@echo "LINK =>  $@"
$(Signed_CryptoEnclave_Name): $(CryptoEnclave_Name)
	@$(SGX_ENCLAVE_SIGNER) sign -key CryptoEnclave/CryptoEnclave_private.pem -enclave $(CryptoEnclave_Name) -out $@ -config $(CryptoEnclave_Config_File)
	@echo "SIGN =>  $@"

######## Clean / Rebuild ########
# 必须删除 App 侧 .o：旧版 clean 只清 Enclave 对象，会导致 Server/RPC 等
# 修改 .cpp 后仍链接旧逻辑（encdb_server 行为与源码不一致）。

clean:
	@rm -f $(Client_App_Name) $(Server_App_Name) $(Benchmark_App_Name)
	@rm -f $(CryptoEnclave_Name) $(Signed_CryptoEnclave_Name)
	@rm -f $(App_All_Cpp_Objects)
	@rm -f $(App_Generated_Files)
	@rm -f $(CryptoEnclave_Cpp_Objects) $(Enclave_Generated_Files)
	@rm -f CryptoTestingApp/*.o CryptoEnclave/*.o
	@echo "CLEAN => removed binaries, all .o, and edger8r generated files"

rebuild: clean all
