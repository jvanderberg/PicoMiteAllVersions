/*
 * shared/net/mm_net_json_fun.c - JSON$() BASIC function.
 *
 * Hardware-independent: parses a JSON document held in an integer array
 * (as filled by WEB TCP CLIENT REQUEST) and extracts one item by a
 * dotted/indexed path expression. Linked by every port that registers
 * the Json$( token.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "cJSON.h"

void fun_json(void) {
    char * json_string = NULL;
    const cJSON * root = NULL;
    void * ptr1 = NULL;
    char * p;
    sret = GetTempMemory(STRINGSIZE);
    int64_t * dest = NULL;
    MMFLOAT tempd;
    int i, j, k, mode, index;
    char field[32], num[6];
    getargs(&ep, 3, (unsigned char *)",");
    char * a = GetTempMemory(STRINGSIZE);
    ptr1 = findvar(argv[0], V_FIND | V_EMPTY_OK);
    if (g_vartbl[g_VarIndex].type & T_INT) {
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].dims[0] <= 0) {
            error("Argument 1 must be integer array");
        }
        dest = (int64_t *)ptr1;
        json_string = (char *)&dest[1];
    } else
        error("Argument 1 must be integer array");
    cJSON_InitHooks(NULL);
    cJSON * parse = cJSON_Parse(json_string);
    if (parse == NULL) error("Invalid JSON data");
    root = parse;
    p = (char *)getCstring((unsigned char *)argv[2]);
    int len = strlen(p);
    memset(field, 0, 32);
    memset(num, 0, 6);
    i = 0;
    j = 0;
    k = 0;
    mode = 0;
    while (i < len) {
        if (p[i] == '[') {
            mode = 1;
            field[j] = 0;
            root = cJSON_GetObjectItemCaseSensitive(root, field);
            memset(field, 0, 32);
            j = 0;
        }
        if (p[i] == ']') {
            num[k] = 0;
            index = atoi(num);
            root = cJSON_GetArrayItem(root, index);
            memset(num, 0, 6);
            k = 0;
        }
        if (p[i] == '.') {
            if (mode == 0) {
                field[j] = 0;
                root = cJSON_GetObjectItemCaseSensitive(root, field);
                memset(field, 0, 32);
                j = 0;
            } else {
                mode = 0;
            }
        } else {
            if (mode == 0)
                field[j++] = p[i];
            else if (p[i] != '[')
                num[k++] = p[i];
        }
        i++;
    }
    /* A path ending in "]" already resolved to the array element; a final
     * lookup with the empty field name would discard it. */
    if (field[0]) root = cJSON_GetObjectItem(root, field);

    if (cJSON_IsObject(root)) {
        cJSON_Delete(parse);
        error("Not an item");
        return;
    }
    if (cJSON_IsInvalid(root)) {
        cJSON_Delete(parse);
        error("Not an item");
        return;
    }
    if (cJSON_IsNumber(root)) {
        tempd = root->valuedouble;
        if ((MMFLOAT)((int64_t)tempd) == tempd)
            IntToStr(a, (int64_t)tempd, 10);
        else
            FloatToStr(a, tempd, 0, STR_AUTO_PRECISION, ' ');
        cJSON_Delete(parse);
        sret = (unsigned char *)a;
        sret = CtoM(sret);
        targ = T_STR;
        return;
    }
    if (cJSON_IsBool(root)) {
        int64_t tempint = root->valueint;
        cJSON_Delete(parse);
        if (tempint)
            strcpy((char *)sret, "true");
        else
            strcpy((char *)sret, "false");
        sret = CtoM(sret);
        targ = T_STR;
        return;
    }
    if (cJSON_IsString(root)) {
        strcpy(a, root->valuestring);
        cJSON_Delete(parse);
        sret = (unsigned char *)a;
        sret = CtoM(sret);
        targ = T_STR;
        return;
    }
    cJSON_Delete(parse);
    targ = T_STR;
    sret = (unsigned char *)a;
}
