#ifndef SCENARIO_MANAGER_H
#define SCENARIO_MANAGER_H

#define MAX_SCENARIO_MSG_LEN 256

// scenario.csv의 한 행에 해당하는 규칙 구조체
typedef struct {
    int event_id;
    int entry_direction_code;       // 0일 시 해당없음. 값이 1일 때는 타겟 방향 코드 중 첫 번째 값, 2는 두번째 값 . . .
    int egress_direction_code;      // 0일 시 해당없음
    int conflict_direction_code;    // 0일 시 해당없음
    int Agroup;                     // 첫 번째 타겟 그룹 번호
    int Agroup2;                    // 첫 번째 타켓 그룹 번호 + 1000
    int Bgroup;
    int Bgroup2;
    int Cgroup;
    int Cgroup2;
    int Dgroup;
    int Dgroup2;
} VMS_ScenarioRule_t;

// 시나리오 규칙들의 리스트
typedef struct {
    VMS_ScenarioRule_t* rules;
    int count;
} VMS_ScenarioList_t;

VMS_ScenarioList_t* load_scenarios_from_csv(const char* csv_filepath);

void free_scenario_list(VMS_ScenarioList_t* list);

#endif // SCENARIO_MANAGER_H
