#include "EnclaveUtils.h"
#include "CryptoEnclave_t.h"

#include "sgx_trts.h"
#include "sgx_tcrypto.h"
#include "../common/data_type.h"
#include "../CryptoTestingApp/EncDBRequest.h"
// #include <bitset> 

void printf( const char *fmt, ...)
{
    char buf[BUFSIZ] = {'\0'};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, BUFSIZ, fmt, ap);
    va_end(ap);
    ocall_print_string(buf);
}

void print_bytes(uint8_t *ptr, uint32_t len) {
  for (uint32_t i = 0; i < len; i++) {
    printf("%x", *(ptr + i));
  }

  printf("\n");
}


int  cmp(const uint8_t *value1, const uint8_t *value2, uint32_t len){
    for (uint32_t i = 0; i < len; i++) {
        if (*(value1+i) != *(value2+i)) {
        return -1;
        }
    }

    return 0;
}

void  clear(uint8_t *dest, uint32_t len){
    for (uint32_t i = 0; i < len; i++) {
        *(dest + i) = 0;
    }
}

std::string pack_doc_bundle_v1(const std::string& index, const std::string& body) {
    std::string out(MAX_FILE_SIZE, '\0');
    out[0] = static_cast<char>(DOC_BUNDLE_MAGIC_V1);
    size_t off = 1;

    const uint32_t index_len = static_cast<uint32_t>(index.size());
    memcpy(&out[off], &index_len, sizeof(index_len));
    off += sizeof(index_len);
    if (index_len > 0) {
        memcpy(&out[off], index.data(), index_len);
        off += index_len;
    }

    const uint32_t body_len = static_cast<uint32_t>(body.size());
    memcpy(&out[off], &body_len, sizeof(body_len));
    off += sizeof(body_len);
    if (body_len > 0) {
        memcpy(&out[off], body.data(), body_len);
    }
    return out;
}

std::string pack_doc_bundle_legacy(const std::string& content) {
    std::string out(MAX_FILE_SIZE, '\0');
    out[0] = static_cast<char>(DOC_BUNDLE_MAGIC_LEGACY);
    const size_t copy_len = content.size() < (MAX_FILE_SIZE - 1) ? content.size() : (MAX_FILE_SIZE - 1);
    if (copy_len > 0) {
        memcpy(&out[1], content.data(), copy_len);
    }
    return out;
}

static void fill_doc_bundle_padding(
    std::string& out,
    size_t data_end,
    const void* enc_key,
    const std::string& doc_id,
    uint32_t slot_version
) {
    if (data_end >= MAX_FILE_SIZE) {
        return;
    }
    size_t off = data_end;
    uint64_t ctr = 0;
    unsigned char block[HASH_VALUE_LEN_128];
    while (off < MAX_FILE_SIZE) {
        const std::string msg =
            doc_id + std::to_string(slot_version) + "pad" + std::to_string(ctr);
        hash_SHA128(enc_key, msg.c_str(), static_cast<int>(msg.size()), block);
        const size_t n = std::min(sizeof(block), MAX_FILE_SIZE - off);
        memcpy(&out[off], block, n);
        off += n;
        ++ctr;
    }
}

std::string pack_doc_bundle_v2(
    const std::string& index,
    const std::string& body,
    uint32_t slot_version,
    const void* enc_key,
    const std::string& doc_id
) {
    std::string out(MAX_FILE_SIZE, '\0');
    out[0] = static_cast<char>(DOC_BUNDLE_MAGIC_V2);
    size_t off = 1;
    memcpy(&out[off], &slot_version, sizeof(slot_version));
    off += sizeof(slot_version);

    const uint32_t index_len = static_cast<uint32_t>(index.size());
    memcpy(&out[off], &index_len, sizeof(index_len));
    off += sizeof(index_len);
    if (index_len > 0) {
        memcpy(&out[off], index.data(), index_len);
        off += index_len;
    }

    const uint32_t body_len = static_cast<uint32_t>(body.size());
    memcpy(&out[off], &body_len, sizeof(body_len));
    off += sizeof(body_len);
    if (body_len > 0) {
        memcpy(&out[off], body.data(), body_len);
        off += body_len;
    }

    fill_doc_bundle_padding(out, off, enc_key, doc_id, slot_version);
    return out;
}

std::string pack_doc_tombstone_v2(
    uint32_t slot_version,
    const void* enc_key,
    const std::string& doc_id
) {
    return pack_doc_bundle_v2(std::string(), std::string(), slot_version, enc_key, doc_id);
}

bool parse_doc_bundle_ex(
    const std::string& plain,
    std::string& index_out,
    std::string& body_out,
    uint32_t* slot_version_out
) {
    index_out.clear();
    body_out.clear();
    if (slot_version_out != nullptr) {
        *slot_version_out = 0;
    }
    if (plain.empty()) {
        return false;
    }

    const uint8_t magic = static_cast<uint8_t>(plain[0]);
    if (magic == DOC_BUNDLE_MAGIC_LEGACY) {
        if (plain.size() > 1) {
            index_out.assign(plain.data() + 1, plain.size() - 1);
            const size_t end = index_out.find('\0');
            if (end != std::string::npos) {
                index_out.resize(end);
            }
        }
        return true;
    }

    size_t off = 1;
    if (magic == DOC_BUNDLE_MAGIC_V2) {
        if (plain.size() < off + sizeof(uint32_t) + 2 * sizeof(uint32_t)) {
            return false;
        }
        uint32_t slot_version = 0;
        memcpy(&slot_version, plain.data() + off, sizeof(slot_version));
        off += sizeof(slot_version);
        if (slot_version_out != nullptr) {
            *slot_version_out = slot_version;
        }
    } else if (magic != DOC_BUNDLE_MAGIC_V1) {
        body_out = plain;
        return false;
    }

    if (plain.size() < off + 2 * sizeof(uint32_t)) {
        return false;
    }

    uint32_t index_len = 0;
    memcpy(&index_len, plain.data() + off, sizeof(index_len));
    off += sizeof(index_len);
    if (off + index_len > plain.size()) {
        return false;
    }
    if (index_len > 0) {
        index_out.assign(plain.data() + off, index_len);
        off += index_len;
    }

    if (off + sizeof(uint32_t) > plain.size()) {
        return false;
    }
    uint32_t body_len = 0;
    memcpy(&body_len, plain.data() + off, sizeof(body_len));
    off += sizeof(body_len);
    if (off + body_len > plain.size()) {
        return false;
    }
    if (body_len > 0) {
        body_out.assign(plain.data() + off, body_len);
    }
    return true;
}

bool parse_doc_bundle(const std::string& plain, std::string& index_out, std::string& body_out) {
    return parse_doc_bundle_ex(plain, index_out, body_out, nullptr);
}

std::vector<std::string> wordTokenize(const char *content, int content_length) {
    char delim[] = ",";//" ,.-";
    std::vector<std::string> result;

    char *content_cpy = (char*)malloc(content_length);
    memcpy(content_cpy,content,content_length);

    char *token = strtok(content_cpy,delim);
    while (token != NULL )
    {
            result.push_back(token); 
            token =  strtok(NULL,delim);
    }

    free(token);
    free(content_cpy);
    //the last , will be counted
    //result.erase(result.end()-1);

    return result;
}

//PRF
void prf_F_improve(const void *key,const void *plaintext,size_t plaintext_len, entryKey *k ){

    //k->content_length = AESGCM_MAC_SIZE + AESGCM_IV_SIZE + plaintext_len; //important- has to be size_t
	//k->content = (char *) malloc(k->content_length);
	enc_aes_gcm(key,plaintext,plaintext_len,k->content,k->content_length);

}

void prf_Enc_improve(const void *key,const void *plaintext,size_t plaintext_len, entryValue *v){

    //v->message_length = AESGCM_MAC_SIZE + AESGCM_IV_SIZE + plaintext_len; //important- has to be size_t
	//v->message = (char *) malloc(v->message_length);
	enc_aes_gcm(key,plaintext,plaintext_len,v->message,v->message_length);
}


void prf_Dec_Improve(const void *key,const void *ciphertext,size_t ciphertext_len, entryValue *value ){


    //value->message_length = ciphertext_len - AESGCM_MAC_SIZE - AESGCM_IV_SIZE;
	//value->message = (char *) malloc(value->message_length);
    dec_aes_gcm(key,ciphertext,ciphertext_len,value->message,value->message_length);
}

void enc_aes_gcm(const void *key, const void *plaintext, size_t plaintext_len, void *ciphertext, size_t ciphertext_len)
{
  uint8_t p_dst[ciphertext_len] = {0};

  //p_dst = mac + iv + cipher
	sgx_rijndael128GCM_encrypt(
    (sgx_aes_gcm_128bit_key_t*)key,
		(uint8_t *) plaintext, plaintext_len,
		p_dst + AESGCM_MAC_SIZE + AESGCM_IV_SIZE, //where  the cipher should be stored
		(uint8_t *)gcm_iv, AESGCM_IV_SIZE,
		NULL, 0,
		(sgx_aes_gcm_128bit_tag_t *) p_dst);	//the tag should be the first 16 bytes and auto dumped out

  memcpy(p_dst + AESGCM_MAC_SIZE, gcm_iv, AESGCM_IV_SIZE);

  //copy tag+iv+cipher to ciphertext
  memcpy(ciphertext,p_dst,ciphertext_len);

}

void dec_aes_gcm(const void *key, const void *ciphertext, size_t ciphertext_len, void *plaintext, size_t plaintext_len){
    
    uint8_t p_dst[plaintext_len] = {0};

	sgx_status_t ret = sgx_rijndael128GCM_decrypt(
		(sgx_aes_gcm_128bit_key_t*)key,
		((uint8_t *)ciphertext + AESGCM_MAC_SIZE + AESGCM_IV_SIZE), ciphertext_len-AESGCM_MAC_SIZE-AESGCM_IV_SIZE,
		p_dst,
		(uint8_t *)gcm_iv, AESGCM_IV_SIZE,
		NULL, 0,
		(sgx_aes_gcm_128bit_tag_t *) ciphertext); //get the first 16 bit tag to verify
    
    if (ret != SGX_SUCCESS) {
        printf("dec error %d", ret);
        printf("ciphertext %s", ((uint8_t *)ciphertext + AESGCM_MAC_SIZE + AESGCM_IV_SIZE));
        printf("ciphertext_len %d", ciphertext_len);
        printf("plaintext_len %d", plaintext_len);
    }
	memcpy(plaintext, p_dst, plaintext_len);
}

//generating 128bit output digest
int hash_SHA128(const void *key, const void *msg, int msg_len, void *value){
    
    sgx_status_t ret = SGX_ERROR_UNEXPECTED;
    ret = sgx_rijndael128_cmac_msg(
            (sgx_cmac_128bit_key_t *)key,
            (const uint8_t*)msg,
            msg_len,
            (sgx_cmac_128bit_tag_t*)value);
     
    if (ret == SGX_SUCCESS) {
        return 0;
    }
    else {
        printf("[*] hash error: %d\n", ret);
        return 1;
    }  
}

//make sure the key is 16 bytes and appended to the digest
int hash_SHA128_key(const void *key, int key_len, const void *msg, int msg_len, void *value){
    
    int result;
    result = hash_SHA128(key,msg,msg_len,value);
    if (result==0) {
        memcpy((uint8_t *)value+ENTRY_HASH_KEY_LEN_128,(const uint8_t *)key,key_len);
        return 0;
    } else{
        printf("[*] hash error: %d\n", result);
        return 1;
    }
}
