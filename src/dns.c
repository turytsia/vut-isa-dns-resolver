/**
 * @file dns.c
 * @brief DNS Query Utility
 *
 * This C source file, "dns_query.c" provides a utility for sending DNS queries
 * to a specified DNS server and processing the responses. It supports various query
 * types, including A, AAAA, PTR queries, and reverse DNS queries. The utility allows
 * you to query both IPv4 and IPv6 DNS servers.
 *
 * @author Oleksandr Turytsia (xturyt00)
 * @date October 18, 2023
 */
#include "dns.h"

int main(int argc, char** argv) {

    args_t args;

    memset(&args, 0, sizeof(args_t));   // Reset args

    // Read and validate program arguments
    args_err_t args_err_code = getopts(&args, argc, argv);
    switch(args_err_code){
        case E_UNKNOWN_OPT:
            exit_error(args_err_code, "Unknown option");
            break;
        case E_PORT_INV:
            exit_error(args_err_code, "Port is not valid (1-65535)");
            break;
        case E_PORT_MISS:
            exit_error(args_err_code, "Port is missing for the option -p");
            break;
        case E_SRC_MISS:
            exit_error(args_err_code, "Source address is missing for the options -s");
            break;
        case E_TGT_MISS:
            exit_error(args_err_code, "Target address is not specified");
            break;
        case E_OPT_DOUBLE:
            exit_error(args_err_code, "You have specified the same option twice");
            break;
    }

    struct addrinfo hints, *res;        // Hints and result list for getaddrinfo

    memset(&hints, 0, sizeof(struct addrinfo));   // Reset hints
    hints.ai_family = AF_UNSPEC;        // Allow IPV6 or IPV4
    hints.ai_socktype = SOCK_DGRAM;     // UDP

    // Get ip address of a specified dns server
    int err = getaddrinfo(args.source_addr, args.port, &hints, &res);
    if (err) {
        if (err == EAI_SYSTEM){
            exit_error(E_EAI, strerror(errno));
        }
        else{
            exit_error(E_GAI, gai_strerror(err));
        }
    }

    // Buffer for ip address of a specified dns server
    char dns_addr[args.ipv6 ? INET6_ADDRSTRLEN : INET_ADDRSTRLEN];

    // Save ip address of a dns server into the buffer
    if (res->ai_family == AF_INET6) {
        struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)res->ai_addr;
        inet_ntop(AF_INET6, &(ipv6->sin6_addr), dns_addr, INET6_ADDRSTRLEN);
    }
    else if (res->ai_family == AF_INET) {
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &(ipv4->sin_addr), dns_addr, INET_ADDRSTRLEN);
    }
    else {
        exit_error(E_FAMILY, "Unsupported address family");
    }

    // Dns query buffer
    unsigned char query[MAX_BUFF] = { 0 };

    // Construct DNS query and save it into the buffer
    create_dns_query(&args, query);

    const int dns_header_size = sizeof(dns_header_t);
    const int dns_question_size = sizeof(dns_question_t);
    const int qname_size = (strlen(((char*)query + dns_header_size)) + 1);
    const int query_size = (dns_header_size + qname_size + dns_question_size);

    // Buffer to store received data
    unsigned char buffer[MAX_BUFF] = { 0 };

    send_query_err_t send_err_code = send_dns_query(&args, res->ai_family, buffer, query, dns_addr, query_size);
    switch(send_err_code){
        case E_SOCK:
            freeaddrinfo(res);
            exit_error(send_err_code, "Socket creation failed");
            break;
        case E_SENDTO:
            freeaddrinfo(res);
            exit_error(send_err_code, "DNS query sendto failed");
            break;
        case E_TIMEOUT:
            freeaddrinfo(res);
            exit_error(send_err_code, "Receive timeout reached. No data received");
            break;
        case E_RECVFROM:
            freeaddrinfo(res);
            exit_error(send_err_code, "DNS query recvfrom failed");
            break;
    }

    // Clean up getaddrinfo
    freeaddrinfo(res);

    // Extract the DNS header
    dns_header_t* dns_header = (dns_header_t*)buffer;
    
    switch(dns_header->rcode){
        case RCODE_FORMAT_ERROR:
            exit_error(E_FORMAT, "RCODE 1, Format error");
            break;
        case RCODE_SERVER_FAILURE:
            exit_error(E_SERVER_FAIL, "RCODE 2, Server failure");
            break;
        case RCODE_NAME_ERROR:
            exit_error(E_NAME, "RCODE 3, Name error");
            break;
        case RCODE_NOT_IMPLEMENTED:
            exit_error(E_NOT_IMPL, "RCODE 4, Not implemented");
            break;
        case RCODE_REFUCED:
            exit_error(E_REFUSED, "RCODE 5, Refused");
            break;
    }

    // Extract the DNS question
    dns_question_t* dns_question = (dns_question_t*)(buffer + dns_header_size + qname_size);

    unsigned short qtype = ntohs(dns_question->qtype);
    unsigned short qclass = ntohs(dns_question->qclass);

    // Set pointer to QNAME
    unsigned char* pointer = (unsigned char*)(buffer + dns_header_size);

    printf("Authoritative: %s, Recursive: %s, Truncated: %s\n",  bool_to_yes_no(dns_header->aa), bool_to_yes_no(dns_header->rd), bool_to_yes_no(dns_header->tc));
    printf("Question section (%d)\n", htons(dns_header->qdcount));

    char qname[MAX_BUFF] = {0};

    parse_domain_name(pointer, buffer, qname);

    printf(" %s, %s, %s\n", qname, get_dns_type(qtype), get_dns_class(qclass));

    pointer += qname_size + dns_question_size;

    printf("Answer section (%d)\n", htons(dns_header->ancount));
    print_rr(pointer, buffer, htons(dns_header->ancount), args.test);

    printf("Authority section (%d)\n", htons(dns_header->nscount));
    print_rr(pointer, buffer, htons(dns_header->nscount), args.test);

    printf("Additional section (%d)\n", htons(dns_header->arcount));
    print_rr(pointer, buffer, htons(dns_header->arcount), args.test);

    return 0;
}

/**
 * @brief Print DNS Resource Records (RRs)
 *
 * This function prints DNS resource records (RRs) based on the information provided in the buffer.
 * It iterates through the RRs and prints their details, such as name, type, class, TTL, and data content.
 *
 * @param pointer Pointer to the beginning of the RRs section.
 * @param buffer Pointer to the DNS packet buffer.
 * @param n Number of RRs to print.
 */
void print_rr(unsigned char* pointer, unsigned char* buffer, int n, int is_test) {
    for (int i = 0; i < n; i++) {
        char name[MAX_NAME] = { 0 };

        parse_domain_name(pointer, buffer, name);

        pointer += get_name_length(pointer, name);

        dns_rr_t* dns_rr = (dns_rr_t*)(pointer);

        unsigned short rr_type = ntohs(dns_rr->type);
        unsigned short rr_class = ntohs(dns_rr->class);
        unsigned int rr_ttl = ntohl(dns_rr->ttl);
        unsigned short rr_rdlength = ntohs(dns_rr->rdlength);

        printf(" %s, %s, %s, %d, ", name, get_dns_type(rr_type), get_dns_class(rr_class), is_test ? 0 : rr_ttl);

        switch (rr_type) {
            case A:
                print_ipv4_data(pointer);
                break;
            case CNAME:
            case PTR:
                print_domain_name_data(pointer, buffer);
                break;
            case AAAA:
                print_ipv6_data(pointer);
                break;
            case SOA:
                print_soa_data(((unsigned char*)dns_rr + sizeof(dns_rr_t)), buffer);
                break;
            default:
                printf("%s is not supported yet.\n", get_dns_type(rr_type));
        }

        pointer +=  sizeof(dns_rr_t) + rr_rdlength;
    }
}

/**
 * @brief Print IPv4 data from a DNS resource record (A record)
 *
 * This function prints the IPv4 address data from a DNS A record.
 *
 * @param pointer Pointer to the beginning of the RDATA section of the A record.
 */
void print_ipv4_data(unsigned char* pointer) {
    struct in_addr ipv4_addr;
    memcpy(&ipv4_addr, pointer + sizeof(dns_rr_t), sizeof(struct in_addr));
    char ip_address[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ipv4_addr, ip_address, INET_ADDRSTRLEN);
    printf("%s\n", ip_address);
}

/**
 * @brief Print domain name data from a DNS resource record (CNAME or PTR record)
 *
 * This function prints the domain name data from a DNS CNAME or PTR record.
 *
 * @param pointer Pointer to the beginning of the RDATA section of the CNAME or PTR record.
 * @param buffer Pointer to the DNS packet buffer.
 */
void print_domain_name_data(unsigned char* pointer, unsigned char* buffer) {
    char data[MAX_NAME] = { 0 };
    parse_domain_name(pointer + sizeof(dns_rr_t), buffer, data);
    printf("%s\n", data);
}

/**
 * @brief Print IPv6 data from a DNS resource record (AAAA record)
 *
 * This function prints the IPv6 address data from a DNS AAAA record.
 *
 * @param pointer Pointer to the beginning of the RDATA section of the AAAA record.
 */
void print_ipv6_data(unsigned char* pointer) {
    struct in6_addr ipv6_addr;
    memcpy(&ipv6_addr, pointer + sizeof(dns_rr_t), sizeof(struct in6_addr));
    char ip_address[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &ipv6_addr, ip_address, INET6_ADDRSTRLEN);
    printf("%s\n", ip_address);
}

/**
 * @brief Print SOA data from a DNS resource record (SOA record)
 *
 * This function prints the Start of Authority (SOA) data from a DNS SOA record.
 *
 * @param pointer Pointer to the beginning of the RDATA section of the SOA record.
 * @param buffer Pointer to the DNS packet buffer.
 */
void print_soa_data(unsigned char* pointer, unsigned char* buffer) {
    char mname[MAX_NAME] = { 0 };
    char rname[MAX_NAME] = { 0 };

    int mname_len, rname_len;

    parse_domain_name(pointer, buffer, mname);
    mname_len = get_name_length(pointer, mname);

    parse_domain_name(pointer + mname_len, buffer, rname);
    rname_len = get_name_length(pointer + mname_len, rname);

    dns_soa_t* soa = (dns_soa_t*)(pointer + mname_len + rname_len);

    printf("%s, %s, %d, %d, %d, %d, %d\n", mname, rname, ntohl(soa->serial), ntohl(soa->refresh), ntohl(soa->retry), ntohl(soa->expire), ntohl(soa->min_ttl));
}



/**
 * @brief Send a DNS query to a server and receive a response.
 *
 * This function constructs a UDP socket, sends a DNS query to a server, and waits for a response.
 *
 * @param args Pointer to the program's command-line arguments.
 * @param ai_family Address family (AF_INET for IPv4 or AF_INET6 for IPv6).
 * @param buffer Pointer to the buffer for storing the received data.
 * @param query Pointer to the DNS query.
 * @param addr IP address of the target DNS server.
 * @param qlen Length of the query.
 * @return send_query_err_t indicating the result of the query operation.
 */
send_query_err_t send_dns_query(args_t* args, int ai_family, unsigned char* buffer, unsigned char* query, char* addr, int qlen) {
    int sockt;
    int err;
    socklen_t addr_len;
    ssize_t bytes_received;

    struct sockaddr_storage server;

    memset(&server, 0, sizeof(struct sockaddr_storage));

    if (ai_family == AF_INET) {
        struct sockaddr_in* server4 = (struct sockaddr_in*)&server;
        server4->sin_family = AF_INET;
        server4->sin_port = htons(atoi(args->port));
        inet_ntop(AF_INET, &server4->sin_addr, (char*)addr, INET_ADDRSTRLEN);
    }
    else {
        struct sockaddr_in6* server6 = (struct sockaddr_in6*)&server;
        server6->sin6_family = AF_INET6;
        server6->sin6_port = htons(atoi(args->port));
        inet_pton(AF_INET6, (char*)addr, &server6->sin6_addr);
    }

    sockt = socket(ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (sockt == -1) {
        close(sockt);
        return E_SOCK;
    }

    struct timeval timeout;
    timeout.tv_sec = 5;  // Set the timeout in seconds
    timeout.tv_usec = 0;
    setsockopt(sockt, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    err = sendto(sockt, query, qlen, 0, (struct sockaddr*)&server, sizeof(struct sockaddr_storage));
    if (err == -1) {
        close(sockt);
        return E_SENDTO;
    }

    addr_len = sizeof server;
    bytes_received = recvfrom(sockt, buffer, MAX_BUFF, 0, (struct sockaddr*)&server, &addr_len);
    if (bytes_received == -1) {
        close(sockt);
        if (errno == EWOULDBLOCK || errno == EAGAIN) {;
            return E_TIMEOUT;
        } else {
            return E_RECVFROM;
        }
    } 

    close(sockt);
    return 0;
}

/**
 * @brief Parse a domain name from DNS response data.
 *
 * This function parses a domain name from DNS response data and constructs the result
 * in a human-readable format. It handles both regular domain names and domain name compression.
 *
 * @param rdata Pointer to the DNS response data containing the domain name.
 * @param buffer Pointer to the DNS response buffer for handling compression pointers.
 * @param result Buffer to store the parsed domain name as a human-readable string.
 */
void parse_domain_name(unsigned char* rdata, unsigned char* buffer, char* result) {
    unsigned int position = 0;
    unsigned int len;

    while (1) {
        len = rdata[position++];

        // End of the domain name
        if (len == 0) {
            break;
        }

        // Check for message compression (The first two bits are ones)
        // 11XX XXXX & 1100 0000 == 1100 0000
        if ((len & 192) == 192) {
            // A pointer to another location in the packet
            unsigned int offset = rdata[position++];
            // Recursively parse the domain name at the offset
            parse_domain_name(buffer + offset, buffer, result);
            return;
        }
        else {
            for (int i = 0; i < (int)len; i++) {
                result[strlen(result)] = rdata[position++];
            }
            result[strlen(result)] = '.';
        }
    }
}

/**
 * @brief Compress a domain name for DNS response data.
 *
 * P.s Im very proud creator of this function.
 *
 * This function compresses a domain name for inclusion in a DNS response by replacing
 * consecutive periods with a single length byte and a pointer to the domain name's position.
 *
 * @param dest Pointer to the destination buffer where the compressed domain name is stored.
 * @param src Pointer to the source domain name to be compressed.
 * @param len Current length of the domain name part being processed.
 */
void compress(unsigned char* dest, char* src, int len) {
    if (*src == 0) {
        return;
    }

    if (*src == '.') {
        *(dest - len - 1) = len;
        compress(dest + 1, src + 1, 0);
    }
    else {
        *dest = *src;
        compress(dest + 1, src + 1, len + 1);
    }
}

/**
 * @brief Compress a domain name and store it in the destination buffer.
 *
 * This function prepares a domain name for compression and invokes the 'compress' function
 * to compress it for inclusion in a DNS response. The result is stored in the destination buffer.
 *
 * @param dest Pointer to the destination buffer for the compressed domain name.
 * @param src Pointer to the source domain name to be compressed.
 */
void compress_domain_name(unsigned char* dest, char* src) {
    strcat((char*)src, ".");
    compress(dest + 1, src, 0);
}

/**
 * @brief Create a DNS query packet based on program arguments.
 *
 * This function constructs a DNS query packet based on the provided program arguments
 * and stores it in the 'query' buffer.
 *
 * @param args Pointer to the program arguments structure.
 * @param query Pointer to the buffer where the DNS query packet will be stored.
 */
void create_dns_query(args_t* args, unsigned char* query) {
    dns_header_t dns_header = {
        .id = htons(getpid()),      // Set the ID to X (you can change this value)
        .qr = 0,                    // Query
        .opcode = 0,                // Standard query
        .aa = 0,                    // Authoritative 
        .tc = 0,                    // Truncated 
        .rd = args->recursive,      // Recursion Desired
        .ra = 0,                    // Recursion Available
        .z = 0,                     // Reserved
        .cd = 0,
        .ad = 0,
        .rcode = 0,                 // Response code
        .qdcount = htons(1),        // Number of questions, in network byte order
        .ancount = 0,               // Number of answers
        .nscount = 0,               // Number of authority records
        .arcount = 0                // Number of additional records
    };

    // Combine header and question into the final query packet
    memcpy(query, &dns_header, sizeof(dns_header_t));
    
    unsigned char* qname = (unsigned char*)(query + sizeof(dns_header_t));
    unsigned char qbuffer[MAX_BUFF] = {0};

    if(args->reverse) {
        reverse_dns_ipv4((char*)qbuffer, args->target_addr);
    } else {
        strcpy((char*)qbuffer, args->target_addr);
    }

    compress_domain_name(qname, (char*)qbuffer);

    int len = strlen((char*)qname);

    dns_question_t* qinfo = (dns_question_t*)(qname + len + 1);

    qinfo->qtype = htons(args->ipv6 ? AAAA : args->reverse ? PTR : A);
    qinfo->qclass = htons(1);

}

/**
 * @brief Create a reverse DNS domain name for an IPv4 address.
 *
 * This function generates a reverse DNS domain name for the given IPv4 address and
 * stores it in the 'dest' buffer.
 *
 * @param dest Pointer to the destination buffer where the reverse DNS domain name will be stored.
 * @param addr Pointer to the IPv4 address to be reversed.
 */
void reverse_dns_ipv4(char* dest, char* addr) {
    for (char* token = strtok(addr, "."); token != NULL; token = strtok(NULL, ".")) {
        char buf[MAX_BUFF] = { 0 };

        strcpy(buf, (char*)dest);
        if (*buf != 0) {
            sprintf((char*)dest, "%s.%s", token, buf);
        }
        else {
            sprintf((char*)dest, "%s.", token);
        }
    }

    strcat((char*)dest, "in-addr.arpa");
}