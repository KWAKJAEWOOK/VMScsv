#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include <sys/socket.h>
#include <errno.h>

#include "VMSconnection_manager.h"
#include "VMSprotocol.h"
#include "sds_json_types.h"
#include "VMScontroller.h"
#include "scenario_manager.h"

// 스레드 종료를 제어하기 위한 전역 변수 (또는 VMSData 구조체에 포함 가능)
volatile int keep_running_manager = 1;

// vms_manager_manage_connections를 실행할 스레드 함수
void* connection_manager_thread_func(void* arg) {
    VMSServers* servers = (VMSServers*)arg;
    printf("[Thread] Connection manager thread started.\n");
    vms_manager_manage_connections(servers);
    printf("[Thread] Connection manager thread finishing.\n");
    return NULL;
}

// 특정 그룹의 모든 연결된 서버에게 메시지를 전송하는 함수 (뮤텍스 사용)
void send_message_to_group_thread_safe(VMSServers* all_servers, int target_group_id, const char* message, size_t message_len) {
    if (!all_servers || !message || message_len == 0) {
        fprintf(stderr, "[Sender] 잘못된 인자입니다.\n");
        return;
    }

    // 공유 데이터 접근 전 뮤텍스 잠금
    pthread_mutex_lock(&all_servers->mutex);

    VMSServerGroup* group_to_send = NULL;
    for (int i = 0; i < all_servers->num_groups; ++i) {
        if (all_servers->groups[i].group_id == target_group_id) {
            group_to_send = &all_servers->groups[i];
            break;
        }
    }

    if (!group_to_send) {
        fprintf(stderr, "[Sender] 그룹 ID %d 를 찾을 수 없습니다.\n", target_group_id);
        pthread_mutex_unlock(&all_servers->mutex); // 리턴 전 반드시 잠금 해제
        return;
    }

    printf("[Sender] 그룹 %d (%d개 서버)에 메시지 전송 시도 (뮤텍스 잠금 상태)...\n",
           target_group_id, group_to_send->num_servers);

    for (int i = 0; i < group_to_send->num_servers; ++i) {
        VMSServerInfo* server = &group_to_send->servers[i];
        int current_socket_handle = server->socket_handle; // 핸들 값 복사

        if (current_socket_handle != -1) {
            printf("[Sender]   -> %s:%d (그룹 %d, 핸들: %d) 에 전송 중...\n",
                   server->ip_address, server->port, server->group_id_for_log, current_socket_handle);

            ssize_t total_bytes_sent = 0;
            while ((size_t)total_bytes_sent < message_len) {
                ssize_t bytes_sent_this_call = send(current_socket_handle, // 복사된 핸들 사용
                                                    message + total_bytes_sent,
                                                    message_len - total_bytes_sent,
                                                    MSG_NOSIGNAL);

                if (bytes_sent_this_call < 0) {
                    fprintf(stderr, "[Sender]   ERROR: %s:%d 로 전송 실패 (에러: %s).\n",
                           server->ip_address, server->port, strerror(errno));
                    if(server->socket_handle == current_socket_handle) { // 아직 매니저가 바꾸지 않았다면
                        close(server->socket_handle);
                        server->socket_handle = -1;
                    }
                    break; 
                } else if (bytes_sent_this_call == 0) {
                     fprintf(stderr, "[Sender]   WARNING: %s:%d 로 전송 시 0 바이트 전송됨.\n",
                           server->ip_address, server->port);
                    if(server->socket_handle == current_socket_handle) {
                        close(server->socket_handle);
                        server->socket_handle = -1;
                    }
                    break;
                }
                total_bytes_sent += bytes_sent_this_call;
            }
            if ((size_t)total_bytes_sent == message_len) {
                printf("[Sender]   SUCCESS: %s:%d 로 %ld 바이트 전송 완료.\n",
                       server->ip_address, server->port, total_bytes_sent);
            }
        } else {
            printf("[Sender]   SKIP: %s:%d (그룹 %d)는 연결되지 않음 (핸들: -1).\n",
                   server->ip_address, server->port, server->group_id_for_log);
        }
    }
    // 모든 작업 완료 후 뮤텍스 잠금 해제
    pthread_mutex_unlock(&all_servers->mutex);
}

// 파일 내용을 읽어 문자열 버퍼로 반환
char* read_json_file_to_string(const char* filename) {
    FILE *file = NULL;
    char *buffer = NULL;
    long file_size;
    size_t read_size;

    file = fopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error opening JSON file '%s': %s\n", filename, strerror(errno));
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    if (file_size < 0) {
        perror("Error getting file size");
        fclose(file);
        return NULL;
    }
    rewind(file);
    buffer = (char*)malloc(file_size + 1);
    if (buffer == NULL) {
        perror("Failed to allocate memory for JSON string buffer");
        fclose(file);
        return NULL;
    }
    read_size = fread(buffer, 1, file_size, file);
    if (read_size != (size_t)file_size) {
        perror("Failed to read entire JSON file");
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[file_size] = '\0';
    fclose(file);
    return buffer;
}

// 그룹별로 우선순위 높은 메시지 정보를 저장하는 구조체
typedef struct {
    int group_id;
    int message_template_id; // 메시지 템플릿 번호 (1, 2, 3, 4...)
    // 페이로드 생성에 필요한 동적 데이터를 여기에 추가 가능
    double speed;
    double pet;
    int dir_code;
} WinningMessage;

// 구조체 리스트
typedef struct {
    WinningMessage* messages;
    int count;
} WinningMessageList;

// WinningMessageList에 메시지 정보를 업데이트/추가하는 함수
// 동일한 group_id에 대해 더 높은 message_template_id가 들어오면 교체
static void upsert_winning_message(WinningMessageList* list, int group_id, int message_id, const SdsJson_ApproachTrafficInfoData_t* ati) {
    
    // 1. 이미 해당 그룹에 대한 결정이 있는지 확인
    for (int i = 0; i < list->count; ++i) {
        if (list->messages[i].group_id == group_id) {
            // 이미 존재. 메시지 ID가 더 높으면 업데이트
            if (message_id > list->messages[i].message_template_id) {
                list->messages[i].message_template_id = message_id;
                // 페이로드 생성에 필요한 데이터도 함께 업데이트
                if(ati && ati->host_object.num_way_points > 0) {
                     list->messages[i].speed = ati->host_object.way_point_list[0].speed;
                     list->messages[i].dir_code = ati->cvib_dir_code;
                }
                if(ati && ati->has_pet) {
                    list->messages[i].pet = ati->pet;
                }
            }
            return;
        }
    }
    // 2. 해당 그룹에 대한 결정이 없으면 새로 추가
    list->count++;
    WinningMessage* new_array = (WinningMessage*)realloc(list->messages, sizeof(WinningMessage) * list->count);
    if (!new_array) {
        perror("Failed to realloc WinningMessageList");
        list->count--;
        return;
    }
    list->messages = new_array;

    WinningMessage* new_decision = &list->messages[list->count - 1];
    new_decision->group_id = group_id;
    new_decision->message_template_id = message_id;
    // 페이로드 생성에 필요한 데이터 저장
    if(ati && ati->host_object.num_way_points > 0) {
        new_decision->speed = ati->host_object.way_point_list[0].speed;
        new_decision->dir_code = ati->cvib_dir_code;
    }
    if(ati && ati->has_pet) {
        new_decision->pet = ati->pet;
    }
}

// WinningMessageList 메모리 해제 함수
static void free_winning_message_list(WinningMessageList* list) {
    if (!list) return;
    if (list->messages) {
        free(list->messages);
    }
    free(list);
}

int main (int argc, char** argv)
{
    pthread_t conn_manager_tid; // 스레드 ID

    const char* ini_file_name = "vms_servers.ini";
    VMSServers* vms_servers = vms_manager_init(ini_file_name); // 여기서 뮤텍스 초기화됨

    if (vms_servers == NULL) {
        fprintf(stderr, "VMS 매니저 초기화 실패. 프로그램 종료\n");
        return 1;
    }
    printf("VMS 매니저 초기화 성공. 총 설정된 서버 수: %d, 그룹 수: %d\n",
           vms_servers->total_servers_configured, vms_servers->num_groups);

    VMS_TextParamConfig_t config;
    if (!vms_controller_load_config("config.ini", &config)) {
        fprintf(stderr, "Config.ini load failed. Exiting.\n");
        vms_manager_cleanup(vms_servers);
        return 1;
    }

    VMS_ScenarioList_t* scenario_list = load_scenarios_from_csv("scenario.CSV");
    if (!scenario_list) {
        fprintf(stderr, "Scenario CSV load failed. Exiting.\n");
        vms_manager_cleanup(vms_servers);
        return 1;
    }

    // 연결 관리자 스레드 생성
    if (pthread_create(&conn_manager_tid, NULL, connection_manager_thread_func, vms_servers) != 0) {
        perror("Failed to create connection manager thread");
        vms_manager_cleanup(vms_servers); // 뮤텍스도 여기서 destroy됨
        return 1;
    }
    sleep(10);

    // 1. JSON 파일 읽고 파싱 (테스트용)
    char* json_string = read_json_file_to_string("testjson.json");
    if (json_string) {
        SdsJson_MainMessage_t* parsed_message = sds_json_parse_message(json_string);
        if (parsed_message) {
            
            VMS_HostObjectState_List_t* state_list = vms_controller_process_json_to_state(parsed_message, &config);
            WinningMessageList* winning_list = (WinningMessageList*)calloc(1, sizeof(WinningMessageList));
            
            if (state_list && winning_list) {
                // 핵심 로직: 객체 상태와 시나리오 규칙을 비교하여 최종 메시지 결정
                for (int i = 0; i < state_list->count; ++i) {
                    VMS_HostObjectState_t* obj_state = &state_list->hostobjects[i];
                    const SdsJson_ApproachTrafficInfoData_t* original_ati = &parsed_message->approach_traffic_info_list[i];

                    for (int j = 0; j < scenario_list->count; ++j) {
                        VMS_ScenarioRule_t* rule = &scenario_list->rules[j];
                        
                        // 규칙의 방향 코드(1,2,3,4)를 실제 방위각으로 변환
                        int rule_entry_dir = (rule->entry_direction_code > 0) ? config.direction_codes[rule->entry_direction_code - 1] : 0;
                        int rule_egress_dir = (rule->egress_direction_code > 0) ? config.direction_codes[rule->egress_direction_code - 1] : 0;
                        int rule_conflict_dir = (rule->conflict_direction_code > 0) ? config.direction_codes[rule->conflict_direction_code - 1] : 0;

                        // 규칙 매칭 (각각의 객체에 대해 81가지 규칙들을 비교)
                        bool entry_match = (rule_entry_dir == obj_state->entry_direction_code);
                        bool egress_match = (rule_egress_dir == obj_state->egress_direction_code);
                        bool conflict_match = (rule_conflict_dir == obj_state->remote_obj_direction_code);

                        if (entry_match && egress_match && conflict_match) {
                            // A, B, C, D 그룹에 대한 메시지 결정
                            int groups[4] = { config.direction_codes[0], config.direction_codes[1], config.direction_codes[2], config.direction_codes[3] };
                            int group_msgs[4] = { rule->Agroup, rule->Bgroup, rule->Cgroup, rule->Dgroup };
                            int group_msgs2[4] = { rule->Agroup2, rule->Bgroup2, rule->Cgroup2, rule->Dgroup2 };
                            
                            for (int k = 0; k < 4; ++k) {
                                // 기본 그룹에 대한 메시지 결정
                                if (group_msgs[k] >= 0) {
                                    upsert_winning_message(winning_list, groups[k], group_msgs[k], original_ati);
                                }
                                // +1000 그룹에 대한 메시지 결정
                                if (group_msgs2[k] >= 0) {
                                    upsert_winning_message(winning_list, groups[k] + 1000, group_msgs2[k], original_ati);
                                }
                            }
                        }
                    }
                }
                
                // 생성된 최종 메시지 리스트를 기반으로 실제 전송
                printf("\n--- Final Messages to Send ---\n");
                for (int i = 0; i < winning_list->count; ++i) {
                    WinningMessage* msg = &winning_list->messages[i];
                    char payload_buffer[1024];
                    char final_text[512];
                    const char* template = NULL;

                    // 메시지 ID에 따라 템플릿 선택
                    if (msg->message_template_id == 0) template = config.msg_template0;
                    else if (msg->message_template_id == 1) template = config.msg_template1;
                    else if (msg->message_template_id == 2) template = config.msg_template2;
                    else if (msg->message_template_id == 3) template = config.msg_template3;
                    else if (msg->message_template_id == 4) template = config.msg_template4;

                    if (template) {
                        // 템플릿과 저장된 동적 데이터로 최종 텍스트 생성
                        // 각 메시지 ID별로 필요한 인자가 다를 수 있음
                        if(msg->message_template_id == 1 || msg->message_template_id == 3) { // 속도, 방향코드
                             snprintf(final_text, sizeof(final_text), template, msg->dir_code, msg->speed);
                        } else if (msg->message_template_id == 4) { // PET
                            snprintf(final_text, sizeof(final_text), template, msg->pet);
                        } else { // 인자 없는 템플릿
                            snprintf(final_text, sizeof(final_text), "%s", template);
                        }

                        // 전체 페이로드 생성
                        snprintf(payload_buffer, sizeof(payload_buffer), "RST=%s,SPD=%s,TXT=%s%s%s",
                                 config.rst, config.spd, config.default_font, config.default_color, final_text);

                        // 프로토콜 패킷으로 변환하여 전송
                        uint16_t packet_len = 0;
                        uint8_t* packet_data = create_text_control_packet(CMD_TYPE_INSERT, payload_buffer, &packet_len);
                        if (packet_data && packet_len > 0) {
                            printf("==> Sending to Group %d: %s\n", msg->group_id, payload_buffer);
                            send_message_to_group_thread_safe(vms_servers, msg->group_id, (const char*)packet_data, packet_len);
                            free(packet_data);
                        }
                    }
                }
                free_winning_message_list(winning_list);
            }
            if (state_list) free_vms_object_state_list(state_list);
            free_sds_json_main_message(parsed_message);
        }
        free(json_string);
    }
    
    // --- 4. 종료 단계 ---
    printf("메인 스레드: 작업 완료. 연결 관리자 스레드 종료 요청...\n");
    keep_running_manager = 0;
    
    if (pthread_join(conn_manager_tid, NULL) != 0) {
        perror("Failed to join connection manager thread");
    } else {
        printf("메인 스레드: Connection manager thread joined successfully.\n");
    }

    free_scenario_list(scenario_list);
    vms_manager_cleanup(vms_servers);
    printf("모든 작업 완료. 프로그램 종료.\n");
    
    return 0;
}
