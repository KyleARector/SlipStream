#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_trivial_pass(void)
{
    TEST_ASSERT_EQUAL_INT(2, 1 + 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_trivial_pass);
    return UNITY_END();
}
