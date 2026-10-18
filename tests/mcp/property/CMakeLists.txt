# Property workloads currently share the stress executable's seeded generator.
# This target entry is kept separate so a parent build can schedule it independently.
add_test(NAME mcp_property_seeded COMMAND test_stress 1297047635 5000 1)
set_tests_properties(mcp_property_seeded PROPERTIES TIMEOUT 120)
