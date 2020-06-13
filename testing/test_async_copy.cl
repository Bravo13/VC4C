
__kernel
__attribute__((reqd_work_group_size(12,1,1)))
void test_async_copy(const __global int16 *input, __local int16 *output0, __global int16* output1)
{
	size_t size = get_local_size(0);
	event_t event;
	event = async_work_group_copy(output0, input, size, event);
	wait_group_events(1, &event);
	
	unsigned lid = get_local_id(0);
	output1[lid] = output0[lid];
}

__kernel
void test_async_copy_general(const __global int16 *input, __local int16 *output0, __global int16* output1)
{
	size_t size = get_local_size(0);
	event_t event;
	event = async_work_group_copy(output0, input, size, event);
	wait_group_events(1, &event);

	unsigned lid = get_local_id(0);
	output1[lid] = output0[lid];
}
