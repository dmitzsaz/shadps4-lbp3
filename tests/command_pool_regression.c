#include "kk_cmd_pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(3); } } while (0)
#define VKCHECK(x) CHECK((x) == VK_SUCCESS)

static void *VKAPI_PTR poison_alloc(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
   (void)user; (void)scope;
   void *p = NULL;
   if (posix_memalign(&p, alignment < sizeof(void *) ? sizeof(void *) : alignment, size)) return NULL;
   memset(p, 0xa5, size);
   return p;
}
static void VKAPI_PTR poison_free(void *user, void *p) { (void)user; free(p); }
static void *VKAPI_PTR poison_realloc(void *user, void *p, size_t size, size_t align, VkSystemAllocationScope scope) {
   (void)user; (void)align; (void)scope;
   return realloc(p, size);
}
static unsigned free_count(struct kk_cmd_pool *pool) {
   unsigned n = 0;
   for (struct list_head *p = pool->free_bos.next; p != &pool->free_bos; p = p->next) {
      CHECK(++n <= KK_CMD_POOL_BO_MAX);
   }
   CHECK(n == pool->num_free_bos);
   return n;
}
int main(void) {
   VkApplicationInfo ai = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName="LBP3 pool regression", .apiVersion=VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&ai};
   VkInstance instance; VKCHECK(vkCreateInstance(&ici, NULL, &instance));
   uint32_t count = 1; VkPhysicalDevice physical;
   VKCHECK(vkEnumeratePhysicalDevices(instance, &count, &physical)); CHECK(count == 1);
   float priority = 1;
   VkDeviceQueueCreateInfo qci = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=0, .queueCount=1, .pQueuePriorities=&priority};
   VkDeviceCreateInfo dci = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount=1, .pQueueCreateInfos=&qci};
   VkDevice device; VKCHECK(vkCreateDevice(physical, &dci, NULL, &device));
   VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);
   VkAllocationCallbacks alloc = {.pfnAllocation=poison_alloc, .pfnReallocation=poison_realloc, .pfnFree=poison_free};
   VkCommandPoolCreateInfo pci = {.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex=0};
   VkCommandPool pool; VKCHECK(vkCreateCommandPool(device, &pci, &alloc, &pool));
   struct kk_cmd_pool *internal = (struct kk_cmd_pool *)(uintptr_t)pool;
   printf("Initial free count: %u\n", internal->num_free_bos); fflush(stdout);
   CHECK(free_count(internal) == 0);
   const unsigned chunks = 72, chunk_bytes = 65536;
   VkBufferCreateInfo bci = {.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size=chunks*chunk_bytes, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buffer; VKCHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
   VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, buffer, &req);
   VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(physical, &props);
   unsigned mem_type = 0;
   while (mem_type < props.memoryTypeCount && (!(req.memoryTypeBits & (1u << mem_type)) ||
          (props.memoryTypes[mem_type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) != (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) mem_type++;
   CHECK(mem_type < props.memoryTypeCount);
   VkMemoryAllocateInfo mai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize=req.size, .memoryTypeIndex=mem_type};
   VkDeviceMemory memory; VKCHECK(vkAllocateMemory(device, &mai, NULL, &memory));
   VKCHECK(vkBindBufferMemory(device, buffer, memory, 0));
   void *mapped; VKCHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
   VkCommandBufferAllocateInfo cai = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool=pool, .level=VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount=1};
   VkCommandBuffer cmd; VKCHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
   unsigned char data[65536];
   for (unsigned pass=0; pass<3; pass++) {
      memset(data, 0x31+pass, sizeof(data));
      VkCommandBufferBeginInfo begin = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      VKCHECK(vkBeginCommandBuffer(cmd, &begin));
      for (unsigned n=0;n<chunks;n++) vkCmdUpdateBuffer(cmd, buffer, (VkDeviceSize)n*chunk_bytes, chunk_bytes, data);
      VkMemoryBarrier barrier = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
      VKCHECK(vkEndCommandBuffer(cmd));
      VkSubmitInfo si = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount=1, .pCommandBuffers=&cmd};
      VKCHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(queue));
      for (unsigned n=0;n<chunks*chunk_bytes;n++) CHECK(((unsigned char *)mapped)[n] == 0x31+pass);
      VKCHECK(vkResetCommandBuffer(cmd, 0));
      unsigned retained = free_count(internal); CHECK(retained > 0);
      printf("Pass %u: GPU data correct; %u buffers retained\n", pass, retained);
      if (pass == 1) { vkTrimCommandPool(device, pool, 0); CHECK(free_count(internal) == 0); puts("Trim resets list and count"); }
   }
   vkUnmapMemory(device, memory); vkDestroyBuffer(device, buffer, NULL); vkFreeMemory(device, memory, NULL);
   vkDestroyCommandPool(device, pool, &alloc); vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL);
   puts("PASS: poisoned allocation, bounded cache, reuse, trim, post-trim reuse, GPU readback");
   return 0;
}
