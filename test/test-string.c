/* test-string.c — string operations test
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests: strlen, strcpy, strcat, strcmp, strstr, memcpy, ctype. */

#include <stdio.h>
#include <string.h>
#include <ctype.h>

int main(void) {
    const char *str = "Hello, World!";
    printf("strlen(\"%s\") = %zu\n", str, strlen(str));

    char buf[100];
    strcpy(buf, str);
    strcat(buf, " Extra");
    printf("strcat: \"%s\"\n", buf);

    printf("strcmp(\"apple\",\"banana\") = %d\n",
           strcmp("apple", "banana") < 0 ? -1 : 1);

    const char *found = strstr(str, "World");
    printf("strstr: %s\n", found ? found : "NULL");

    printf("tolower('A') = '%c'\n", tolower('A'));

    int nums[] = {1, 2, 3, 4, 5};
    int copy[5];
    memcpy(copy, nums, sizeof(nums));
    printf("memcpy: ");
    for (int i = 0; i < 5; i++) printf("%d ", copy[i]);
    printf("\n");

    return 0;
}
