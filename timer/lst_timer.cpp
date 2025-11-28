#include "lst_timer.h"
#include "../http/http_conn.h"

#include <chrono>

// ============ TimingWheel 实现 ============
TimingWheel::Level1::Level1() : current_slot(0){
    for (int i = 0; i < SLOTS; i++)
        slots[i].head = nullptr;
}

TimingWheel::Level2::Level2() : current_slot(0) {
    for (int i = 0; i < SLOTS; i++)
        slots[i].head = nullptr;
}

TimingWheel::TimingWheel() 
    : enable_level2(false), 
      m_TIMESLOT(5),
      m_min_timeout(15),  // 默认15秒
      m_max_timeout(25),  // 默认25秒
      m_rng(std::random_device{}()),
      m_timeout_dist(m_min_timeout, m_max_timeout) {
}

TimingWheel::~TimingWheel() {
    // 清理 Level1
    for (int i = 0; i < Level1::SLOTS; i++) {
        util_timer* timer = level1.slots[i].head;
        while (timer) {
            util_timer* next = timer->next;
            delete timer;
            timer = next;
        }
    }

    // 清理 Level2
    if (enable_level2) {
        for (int i = 0; i < Level2::SLOTS; i++) {
            util_timer* timer = level2.slots[i].head;
            while (timer) {
                util_timer* next = timer->next;
                delete timer;
                timer = next;
            }
        }
    }
}

void TimingWheel::set_timeslot(int timeslot) {
    m_TIMESLOT = timeslot;
}

void TimingWheel::set_enable_level2(bool enable) {
    enable_level2 = enable;
    if (enable) {
        LOG_INFO("TimingWheel: Level2 enabled (supports up to %d seconds timeout)",
                 Level1::SLOTS * Level2::SLOTS);
    } else {
        LOG_INFO("TimingWheel: Single level mode (supports up to %d seconds timeout)",
                 Level1::SLOTS);
    }
}

void TimingWheel::set_timeout_range(int min_timeout, int max_timeout) {
    if (min_timeout > 0 && max_timeout > min_timeout) {
        m_min_timeout = min_timeout;
        m_max_timeout = max_timeout;
        m_timeout_dist = std::uniform_int_distribution<int>(min_timeout, max_timeout);
        LOG_INFO("TimingWheel: timeout range set to [%d, %d] seconds", 
                 min_timeout, max_timeout);
    } else {
        LOG_WARN("Invalid timeout range: min=%d, max=%d", min_timeout, max_timeout);
    }
}

int TimingWheel::get_random_timeout() {
    return m_timeout_dist(m_rng);
}

void TimingWheel::add_timer(util_timer* timer) {
    if (!timer) return;

    time_t cur = time(nullptr);
    int timeout = timer->expire - cur;

    if (timeout <= 0) {
        timeout = 1;  // 至少1秒
    }

    if (timeout < Level1::SLOTS * Level1::GRANULARITY) {
        // 放入 Level1（当前主要逻辑）
        add_to_level1(timer, timeout);
    }
    else if (enable_level2 && timeout < (Level1::SLOTS * Level1::GRANULARITY) + (Level2::SLOTS * Level2::GRANULARITY)){
        // 【预留】未来启用 Level2 时的逻辑
        add_to_level2(timer, timeout);
    }
    else {
        int slot_idx = (level1.current_slot + Level1::SLOTS - 1) % Level1::SLOTS;
        insert_to_slot_l1(slot_idx, timer);
        LOG_WARN("Timer timeout=%d exceeds capacity, placed in last slot", timeout);
    }
}

void TimingWheel::adjust_timer(util_timer* timer, time_t new_last_active) {
    if (!timer) return;

    // 从旧位置移除
    remove_from_slot(timer);
    
    // 更新时间：使用随机超时
    time_t cur = time(nullptr);
    int random_timeout = get_random_timeout();
    timer->expire = cur + random_timeout;
    timer->last_active = new_last_active;  // 记录活跃时间（用于日志）

    add_timer(timer);
}

void TimingWheel::del_timer(util_timer* timer) {
    if (!timer) return;
    remove_from_slot(timer);
    delete timer;
}

void TimingWheel::tick() {
    if (enable_level2) {
        // 【预留】多级时间轮的 tick 逻辑
        tick_multilevel();
    } else {
        // 当前只处理 Level1
        tick_single_level();
    }
}

// ============ Level1 实现 ============

void TimingWheel::add_to_level1(util_timer* timer, int timeout) {
    // Level1 槽位计算：考虑粒度
    int slots_ahead = timeout / Level1::GRANULARITY;  // timeout / 1
    int slot_idx = (level1.current_slot + slots_ahead) % Level1::SLOTS;
    insert_to_slot_l1(slot_idx, timer);
}

void TimingWheel::tick_single_level() {
    auto start = std::chrono::steady_clock::now();
    
    int slot_idx = level1.current_slot;
    util_timer* timer = level1.slots[slot_idx].head;
    
    time_t cur = time(nullptr);
    std::vector<util_timer*> to_delete;
    
    // 遍历当前槽的所有定时器
    while (timer) {
        util_timer* next = timer->next;
        
        // 去掉懒更新逻辑，直接删除过期的
        if (timer->expire <= cur) {
            to_delete.push_back(timer);
        } else {
            // 理论上不应该有未过期的，记录警告
            LOG_WARN("Unexpired timer in current slot: fd=%d, expire=%ld, cur=%ld",
                     timer->user_data ? timer->user_data->sockfd : -1, 
                     timer->expire, cur);
        }
        
        timer = next;
    }
    
    // 清空当前槽
    level1.slots[slot_idx].head = nullptr;
    
    // 触发回调并删除
    for (auto* t : to_delete) {
        if (t->cb_func) {
            t->cb_func();  // 调用lambda回调
        }
        delete t;
    }
    
    // 推进到下一个槽
    level1.current_slot = (level1.current_slot + 1) % Level1::SLOTS;
    
    //========下面全是日志测试信息
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    LOG_DEBUG("tick_single_level - slot=%d, took=%ld us, deleted=%lu",
              slot_idx, duration.count(), to_delete.size());
    
    // 每隔一段时间输出统计
    static int tick_count = 0;
    static long total_us = 0;
    static long max_us = 0;
    
    tick_count++;
    total_us += duration.count();
    if (duration.count() > max_us) max_us = duration.count();
    
    if (tick_count % 10 == 0) {  // 每10次tick输出一次
        // 统计所有槽的定时器数量
        int total_timers = 0;
        for (int i = 0; i < Level1::SLOTS; i++) {
            util_timer* t = level1.slots[i].head;
            while (t) {
                total_timers++;
                t = t->next;
            }
        }
        
        LOG_INFO("TimingWheel stats - Tick: %d, Avg: %ld us, Max: %ld us, "
                 "Total timers: %d, Deleted: %lu",
                 tick_count, total_us / tick_count, max_us,
                 total_timers, to_delete.size());
    }
}

void TimingWheel::insert_to_slot_l1(int slot_idx, util_timer* timer) {
    timer->next = level1.slots[slot_idx].head;
    timer->prev = nullptr;

    if (level1.slots[slot_idx].head){
        level1.slots[slot_idx].head->prev = timer;
    }

    level1.slots[slot_idx].head = timer;
    timer->wheel_level = 1;
    timer->slot_index = slot_idx;
}

// ============ 时间轮辅助函数 ============

void TimingWheel::remove_from_slot(util_timer* timer) {
    if (timer->prev){
        timer->prev->next = timer->next;
    } else {
        // timer 是链表头，需要更新槽的 head
        if (timer->wheel_level == 1) {
            level1.slots[timer->slot_index].head = timer->next;
        } else if (timer->wheel_level == 2) {
            level2.slots[timer->slot_index].head = timer->next;
        }
    }

    if (timer->next) {
        timer->next->prev = timer->prev;
    }

    timer->prev = nullptr;
    timer->next = nullptr;
}

// Utils类已移至utils/utils.h和utils/utils.cpp 


