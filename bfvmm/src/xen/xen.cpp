//
// Copyright (C) 2019 Assured Information Security, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <bfgpalayout.h>
#include <compiler.h>
#include <mutex>
#include <stdlib.h>

#include <arch/x64/rdtsc.h>
#include <hve/arch/intel_x64/domain.h>
#include <hve/arch/intel_x64/vcpu.h>

#include <pci/cfg.h>
#include <pci/bar.h>
#include <pci/dev.h>

#include <xen/evtchn.h>
#include <xen/gnttab.h>
#include <xen/physdev.h>
#include <xen/sysctl.h>
#include <xen/xenmem.h>
#include <xen/xenver.h>
#include <xen/xen.h>

#include <public/arch-x86/cpuid.h>
#include <public/errno.h>
#include <public/memory.h>
#include <public/platform.h>
#include <public/version.h>
#include <public/hvm/hvm_op.h>
#include <public/hvm/params.h>
#include <public/xsm/flask_op.h>

#define XEN_MAJOR 4UL
#define XEN_MINOR 13UL

/* Taken from xen/include/asm-x86/div64.h */
#define do_div(n, base) ({ \
    uint32_t __base = (base); \
    uint32_t __rem; \
    __rem = ((uint64_t)(n)) % __base; \
    (n) = ((uint64_t)(n)) / __base; \
    __rem; \
})

namespace microv {

static std::mutex xen_mutex;
static uint32_t xen_domid = 0;
static uint32_t xen_vcpuid = 0;
static uint32_t xen_apicid = 0;
static uint32_t xen_acpiid = 0;

static constexpr auto self_ipi_msr = 0x83F;
static constexpr auto hcall_page_msr = 0xC0000500;
static constexpr auto xen_leaf_base = 0x40000100;
static constexpr auto xen_leaf(int i) { return xen_leaf_base + i; }

static void make_xen_ids(xen_domain *dom, xen *xen)
{
    if (dom->initdom()) {
        xen->domid = 0;
        xen->vcpuid = 0;
        xen->apicid = 0;
        xen->acpiid = 0;
        return;
    } else {
        std::lock_guard<std::mutex> lock(xen_mutex);
        xen->domid = ++xen_domid;

        /**
         * Linux uses the vcpuid to index into the vcpu_info array in the
         * struct shared_info page. However it hardcodes vcpu_info[0] in
         * xen_tsc_khz which is used to calibrate the TSC at early boot.
         * This means that we can't index into vcpu_info with vcpuid; it
         * has to be zero. Otherwise, xen_tsc_khz calls pvclock_tsc_khz,
         * which ends up doing a div by zero since vcpu_info[0] is 0.
         */
        //xen->vcpuid = ++xen_vcpuid;
        //xen->apicid = ++xen_apicid;
        //xen->acpiid = ++xen_acpiid;
        xen->vcpuid = 0;
        xen->apicid = 0;
        xen->acpiid = 0;

        ensures(xen->vcpuid < XEN_LEGACY_MAX_VCPUS);
    }
}

/*
 * ns <-> tsc conversion (copied from public/xen.h):
 *
 * ns = ((ticks << tsc_shift) * tsc_to_system_mul) >> 32
 * ns << 32 = (ticks << tsc_shift) * tsc_to_system_mul
 * ((ns << 32) / tsc_to_system_mul) = ticks << tsc_shift
 * ((ns << 32) / tsc_to_system_mul) >> tsc_shift = ticks
 *
 * CPU frequency (Hz):
 *   ((10^9 << 32) / tsc_to_system_mul) >> tsc_shift
 */

static constexpr uint64_t s_to_ns(uint64_t sec)
{
    return sec * 1000000000ULL;
}

static inline uint64_t tsc_to_ns(uint64_t ticks, uint64_t shft, uint64_t mult)
{
    return ((ticks << shft) * mult) >> 32;
}

static inline uint64_t ns_to_tsc(uint64_t ns, uint64_t shft, uint64_t mult)
{
    return ((ns << 32) / mult) >> shft;
}

static inline uint64_t tsc_to_pet(uint64_t tsc, uint64_t pet_shift)
{
    return tsc >> pet_shift;
}

static bool handle_exception(base_vcpu *vcpu)
{
    namespace int_info = vmcs_n::vm_exit_interruption_information;

    auto info = int_info::get();
    auto type = int_info::interruption_type::get(info);

    if (type == int_info::interruption_type::non_maskable_interrupt) {
        return false;
    }

    auto vec = int_info::vector::get(info);
    bfdebug_info(0, "Guest exception");
    bfdebug_subnhex(0, "vector", vec);
    bfdebug_subnhex(0, "rip", vcpu->rip());

    auto rip = vcpu->map_gva_4k<uint8_t>(vcpu->rip(), 32);
    auto buf = rip.get();

    printf("        - bytes: ");
    for (auto i = 0; i < 32; i++) {
        printf("%02x", buf[i]);
    }
    printf("\n");

    vmcs_n::exception_bitmap::set(0);

    return true;
}

static bool handle_tsc_deadline(base_vcpu *vcpu, wrmsr_handler::info_t &info)
{
    bfalert_info(0, "TSC deadline write after SSHOTTMR set");
    return true;
}

static bool xen_leaf0(base_vcpu *vcpu)
{
    vcpu->set_rax(xen_leaf(5));
    vcpu->set_rbx(XEN_CPUID_SIGNATURE_EBX);
    vcpu->set_rcx(XEN_CPUID_SIGNATURE_ECX);
    vcpu->set_rdx(XEN_CPUID_SIGNATURE_EDX);

    vcpu->advance();
    return true;
}

static bool xen_leaf1(base_vcpu *vcpu)
{
    vcpu->set_rax(0x00040D00);
    vcpu->set_rbx(0);
    vcpu->set_rcx(0);
    vcpu->set_rdx(0);

    vcpu->advance();
    return true;
}

static bool xen_leaf2(base_vcpu *vcpu)
{
    vcpu->set_rax(1);
    vcpu->set_rbx(hcall_page_msr);
    vcpu->set_rcx(0);
    vcpu->set_rdx(0);

    vcpu->advance();
    return true;
}

bool xen::xen_leaf4(base_vcpu *vcpu)
{
    uint32_t rax = 0;

//  rax |= XEN_HVM_CPUID_APIC_ACCESS_VIRT;
    rax |= XEN_HVM_CPUID_X2APIC_VIRT;
//  rax |= XEN_HVM_CPUID_IOMMU_MAPPINGS;
    rax |= XEN_HVM_CPUID_VCPU_ID_PRESENT;
    rax |= XEN_HVM_CPUID_DOMID_PRESENT;

    vcpu->set_rax(rax);

    /* These ID values are *not* the same as the microv ones */
    vcpu->set_rbx(this->vcpuid);
    vcpu->set_rcx(this->domid);

    vcpu->advance();
    return true;
}

static bool wrmsr_hcall_page(base_vcpu *vcpu, wrmsr_handler::info_t &info)
{
    auto map = vcpu->map_gpa_4k<uint8_t>(info.val);
    auto buf = gsl::span(map.get(), 0x1000);

    for (uint8_t i = 0; i < 55; i++) {
        auto entry = buf.subspan(i * 32, 32);

        entry[0] = 0xB8U;
        entry[1] = i;
        entry[2] = 0U;
        entry[3] = 0U;
        entry[4] = 0U;
        entry[5] = 0x0FU;
        entry[6] = 0x01U;
        entry[7] = 0xC1U;
        entry[8] = 0xC3U;
    }

    return true;
}

static bool wrmsr_self_ipi(base_vcpu *vcpu, wrmsr_handler::info_t &info)
{
    vcpu->queue_external_interrupt(info.val);
    return true;
}

bool xen::handle_physdev_op()
{
    try {
        switch (m_vcpu->rdi()) {
        case PHYSDEVOP_pci_device_add:
            return m_physdev->pci_device_add();
        default:
            return false;
        }
    } catchall ({
        return false;
    })
}

bool xen::handle_console_io()
{
    expects(m_dom->initdom());

    uint64_t len = m_vcpu->rsi();
    auto buf = m_vcpu->map_gva_4k<char>(m_vcpu->rdx(), len);

    switch (m_vcpu->rdi()) {
    case CONSOLEIO_read: {
        auto n = m_dom->hvc_rx_get(gsl::span(buf.get(), len));
        m_vcpu->set_rax(n);
//        if (n) {
//            printf("console read: ");
//            for (auto i = 0; i < n; i++) {
//                printf("%c", buf.get()[i]);
//            }
//            printf("\n");
//        }
        return true;
    }
    case CONSOLEIO_write: {
        auto n = m_dom->hvc_tx_put(gsl::span(buf.get(), len));
        m_vcpu->set_rax(n);
        return true;
    }
    default:
        return false;
    }
}

bool xen::handle_memory_op()
{
    try {
        switch (m_vcpu->rdi()) {
        case XENMEM_memory_map:
            return m_xenmem->memory_map();
        case XENMEM_add_to_physmap:
            return m_xenmem->add_to_physmap();
        case XENMEM_decrease_reservation:
            return m_xenmem->decrease_reservation();
        case XENMEM_get_sharing_freed_pages:
            return m_xenmem->get_sharing_freed_pages();
        case XENMEM_get_sharing_shared_pages:
            return m_xenmem->get_sharing_shared_pages();
        default:
            break;
        }
    } catchall ({
        return false;
    })

    return false;
}

bool xen::handle_xen_version()
{
    try {
        switch (m_vcpu->rdi()) {
        case XENVER_version:
            return m_xenver->version();
        case XENVER_extraversion:
            return m_xenver->extraversion();
        case XENVER_compile_info:
            return m_xenver->compile_info();
        case XENVER_capabilities:
            return m_xenver->capabilities();
        case XENVER_changeset:
            return m_xenver->changeset();
        case XENVER_platform_parameters:
            return m_xenver->platform_parameters();
        case XENVER_get_features:
            return m_xenver->get_features();
        case XENVER_pagesize:
            return m_xenver->pagesize();
        case XENVER_guest_handle:
            return m_xenver->guest_handle();
        case XENVER_commandline:
            return m_xenver->commandline();
        case XENVER_build_id:
            return m_xenver->build_id();
        default:
            return false;
        }
    } catchall ({
        return false;
    })
}

static bool valid_cb_via(uint64_t via)
{
    const auto type = (via & HVM_PARAM_CALLBACK_IRQ_TYPE_MASK) >> 56;
    if (type != HVM_PARAM_CALLBACK_TYPE_VECTOR) {
        return false;
    }

    const auto vector = via & 0xFFU;
    if (vector < 0x20U || vector > 0xFFU) {
        return false;
    }

    return true;
}

bool xen::handle_hvm_op()
{
    switch (m_vcpu->rdi()) {
    case HVMOP_set_param:
        try {
            auto arg = m_vcpu->map_arg<xen_hvm_param_t>(m_vcpu->rsi());
            switch (arg->index) {
            case HVM_PARAM_CALLBACK_IRQ:
                if (valid_cb_via(arg->value)) {
                    m_evtchn->set_callback_via(arg->value & 0xFF);
                    m_vcpu->set_rax(0);
                } else {
                    m_vcpu->set_rax(-EINVAL);
                }
                return true;
            default:
                bfalert_info(0, "Unsupported HVM set_param");
                return false;
            }
        } catchall({
            return false;
        })
    case HVMOP_get_param:
        expects(!m_dom->initdom());
//        return false;
//        try {
//            auto arg = m_vcpu->map_arg<xen_hvm_param_t>(m_vcpu->rsi());
//            switch (arg->index) {
//            case HVM_PARAM_CONSOLE_EVTCHN:
//                arg->value = m_evtchn->bind_console();
//                break;
//            case HVM_PARAM_CONSOLE_PFN:
//                m_console = m_vcpu->map_gpa_4k<struct xencons_interface>(PVH_CONSOLE_GPA);
//                arg->value = PVH_CONSOLE_GPA >> 12;
//                break;
//            case HVM_PARAM_STORE_EVTCHN:
//                arg->value = m_evtchn->bind_store();
//                break;
//            case HVM_PARAM_STORE_PFN:
//                m_store = m_vcpu->map_gpa_4k<uint8_t>(PVH_STORE_GPA);
//                arg->value = PVH_STORE_GPA >> 12;
//                break;
//            default:
//                bfalert_nhex(0, "Unsupported HVM get_param:", arg->index);
//                return false;
//            }

            m_vcpu->set_rax(-ENOSYS);
            return true;
        //} catchall({
        //    return false;
        //})
    case HVMOP_pagetable_dying:
        m_vcpu->set_rax(-ENOSYS);
        return true;
    default:
       return false;
    }
}

bool xen::handle_event_channel_op()
{
    try {
        switch (m_vcpu->rdi()) {
        case EVTCHNOP_init_control:
            return m_evtchn->init_control();
        case EVTCHNOP_set_priority:
            return m_evtchn->set_priority();
        case EVTCHNOP_alloc_unbound:
            return m_evtchn->alloc_unbound();
        case EVTCHNOP_expand_array:
            return m_evtchn->expand_array();
        case EVTCHNOP_bind_virq:
            return m_evtchn->bind_virq();
        case EVTCHNOP_send:
            return m_evtchn->send();
        case EVTCHNOP_bind_interdomain:
            return m_evtchn->bind_interdomain();
        case EVTCHNOP_close:
            return m_evtchn->close();
        case EVTCHNOP_bind_vcpu:
            return m_evtchn->bind_vcpu();
        default:
            return false;
        }
    } catchall({
        return false;
    })

    return false;
}

bool xen::handle_sysctl()
{
    auto ctl = m_vcpu->map_arg<xen_sysctl_t>(m_vcpu->rdi());
    return m_sysctl->handle(ctl.get());
}

bool xen::handle_domctl()
{
    auto ctl = m_vcpu->map_arg<xen_domctl_t>(m_vcpu->rdi());
    return m_domctl->handle(ctl.get());
}

bool xen::handle_grant_table_op()
{
    try {
        switch (m_vcpu->rdi()) {
        case GNTTABOP_query_size:
            return m_gnttab->query_size();
        case GNTTABOP_set_version:
            return m_gnttab->set_version();
        default:
            return false;
        }
    } catchall ({
        return false;
    })
}

void xen::update_wallclock(const struct xenpf_settime64 *time)
{
    m_shinfo->wc_version++;
    wmb();

    uint64_t x = s_to_ns(time->secs) + time->nsecs - time->system_time;
    uint32_t y = do_div(x, 1000000000);

    m_shinfo->wc_sec = gsl::narrow_cast<uint32_t>(x);
    m_shinfo->wc_sec_hi = gsl::narrow_cast<uint32_t>(x >> 32);
    m_shinfo->wc_nsec = gsl::narrow_cast<uint32_t>(y);

    wmb();
    m_shinfo->wc_version++;
}

bool xen::handle_platform_op()
{
    auto xpf = m_vcpu->map_arg<xen_platform_op_t>(m_vcpu->rdi());
    if (xpf->interface_version != XENPF_INTERFACE_VERSION) {
        m_vcpu->set_rax(-EACCES);
        return true;
    }

    switch (xpf->cmd) {
    case XENPF_get_cpuinfo: {
        expects(m_dom->initdom());
        struct xenpf_pcpuinfo *info = &xpf->u.pcpu_info;
        info->max_present = 1;
        info->flags = XEN_PCPU_FLAGS_ONLINE;
        info->apic_id = this->apicid;
        info->acpi_id = this->acpiid;
        m_vcpu->set_rax(0);
        return true;
    }
    case XENPF_settime64: {
        const struct xenpf_settime64 *time = &xpf->u.settime64;
        if (time->mbz) {
            m_vcpu->set_rax(-EINVAL);
        } else {
            this->update_wallclock(time);
            m_vcpu->set_rax(0);
        }
        return true;
    }
    default:
        bfalert_ndec(0, "Unimplemented platform op", xpf->cmd);
        return false;
    }
}

bool xen::handle_xsm_op()
{
    expects(m_dom->initdom());
    auto flop = m_vcpu->map_arg<xen_flask_op_t>(m_vcpu->rdi());

    if (flop->interface_version != XEN_FLASK_INTERFACE_VERSION) {
        m_vcpu->set_rax(-EACCES);
        return true;
    }

    switch (flop->cmd) {
    case FLASK_SID_TO_CONTEXT:
        break;
    default:
        bfalert_nhex(0, "unhandled flask_op", flop->cmd);
        break;
    }

    m_vcpu->set_rax(-EACCES);
    return true;
}

struct vcpu_time_info *xen::vcpu_time()
{
    return &m_shinfo->vcpu_info[this->vcpuid].time;
}

void xen::stop_timer()
{
    m_vcpu->disable_preemption_timer();
    m_pet_enabled = false;
}

int xen::set_timer()
{
    auto pet = 0ULL;
    auto vti = this->vcpu_time();
    auto sst = m_vcpu->map_arg<vcpu_set_singleshot_timer_t>(m_vcpu->rdx());

    /* Get the preemption timer ticks corresponding to the deadline */
    if (vti->system_time >= sst->timeout_abs_ns) {
        if (sst->flags & VCPU_SSHOTTMR_future) {
            return -ETIME;
        }
        pet = 0;
    } else {
        auto ns = sst->timeout_abs_ns - vti->system_time;
        auto tsc = ns_to_tsc(ns, vti->tsc_shift, vti->tsc_to_system_mul);
        pet = tsc_to_pet(tsc, m_pet_shift);
    }

    m_vcpu->set_preemption_timer(pet);
    m_vcpu->enable_preemption_timer();
    m_pet_enabled = true;

    return 0;
}

bool xen::handle_vcpu_op()
{
    expects(m_vcpu->rsi() == vcpuid);

    switch (m_vcpu->rdi()) {
    case VCPUOP_stop_periodic_timer:
        m_vcpu->set_rax(0);
        return true;
    case VCPUOP_stop_singleshot_timer:
        this->stop_timer();
        m_vcpu->set_rax(0);
        return true;
    case VCPUOP_set_singleshot_timer:
        m_vcpu->set_rax(this->set_timer());
        if (!m_pet_hdlrs_added) {
            m_vcpu->add_preemption_timer_handler({&xen::handle_pet, this});
            m_vcpu->add_hlt_handler({&xen::handle_hlt, this});
            m_vcpu->add_exit_handler({&xen::vmexit_save_tsc, this});
            m_vcpu->emulate_wrmsr(0x6E0, {handle_tsc_deadline});
            m_pet_hdlrs_added = true;
        }
        return true;
    case VCPUOP_register_vcpu_time_memory_area: {
        expects(m_shinfo);
        auto tma = m_vcpu->map_arg<vcpu_register_time_memory_area_t>(
            m_vcpu->rdx());
        m_user_vti = m_vcpu->map_arg<struct vcpu_time_info>(tma->addr.v);
        memcpy(m_user_vti.get(), this->vcpu_time(), sizeof(*this->vcpu_time()));
        m_vcpu->set_rax(0);
        return true;
    }
    case VCPUOP_register_runstate_memory_area: {
        auto rma = m_vcpu->map_arg<vcpu_register_runstate_memory_area_t>(
            m_vcpu->rdx());
        m_runstate = m_vcpu->map_arg<struct vcpu_runstate_info>(rma->addr.v);
        m_runstate->state = RUNSTATE_running;
        m_runstate->state_entry_time = this->vcpu_time()->system_time;
        m_runstate->time[RUNSTATE_running] = m_runstate->state_entry_time;
        m_vcpu->set_rax(0);
        return true;
    }
    default:
        return false;
    }
}

bool xen::handle_vm_assist()
{
    if (m_vcpu->rdi() != VMASST_CMD_enable) {
        return false;
    }

    switch (m_vcpu->rsi()) {
    case VMASST_TYPE_runstate_update_flag:
        m_runstate_assist = true;
        m_vcpu->set_rax(0);
        return true;
    default:
        return false;
    }
}

void xen::queue_virq(uint32_t virq)
{
    m_evtchn->queue_virq(virq);
}

void xen::update_runstate(int new_state)
{
    if (GSL_UNLIKELY(!m_shinfo)) {
        return;
    }

    /* Update kernel time info */
    auto kvti = this->vcpu_time();
    const uint64_t mult = kvti->tsc_to_system_mul;
    const uint64_t shft = kvti->tsc_shift;
    const uint64_t prev = kvti->tsc_timestamp;

    kvti->version++;
    wmb();
    const auto next = ::x64::read_tsc::get();
    kvti->system_time += tsc_to_ns(next - prev, shft, mult);
    kvti->tsc_timestamp = next;
    wmb();
    kvti->version++;

    if (GSL_UNLIKELY(!m_user_vti)) {
        return;
    }

    /* Update userspace time info */
    auto uvti = m_user_vti.get();

    uvti->version++;
    wmb();
    uvti->system_time = kvti->system_time;
    uvti->tsc_timestamp = next;
    wmb();
    uvti->version++;

    if (GSL_UNLIKELY(!m_runstate)) {
        return;
    }

    /* Update runstate info */
    auto old_state = m_runstate->state;
    auto old_entry = m_runstate->state_entry_time;

    m_runstate->time[old_state] += kvti->system_time - old_entry;
    m_runstate->state = new_state;

    if (GSL_LIKELY(m_runstate_assist)) {
        m_runstate->state_entry_time = XEN_RUNSTATE_UPDATE;
        wmb();
        m_runstate->state_entry_time |= kvti->system_time;
        wmb();
        m_runstate->state_entry_time &= ~XEN_RUNSTATE_UPDATE;
        wmb();
    } else {
        m_runstate->state_entry_time = kvti->system_time;
    }
}

/* Steal ticks from the guest's preemption timer */
void xen::steal_pet_ticks()
{
    if (GSL_UNLIKELY(m_tsc_at_exit == 0)) {
        return;
    }

    auto pet = m_vcpu->get_preemption_timer();
    auto tsc = this->vcpu_time()->tsc_timestamp;
    auto stolen_tsc = tsc - m_tsc_at_exit;
    auto stolen_pet = stolen_tsc >> m_pet_shift;

    pet = (stolen_pet >= pet) ? 0 : pet - stolen_pet;
    m_vcpu->set_preemption_timer(pet);
}

void xen::resume_update(bfobject *obj)
{
    bfignored(obj);

    this->update_runstate(RUNSTATE_running);

    if (m_pet_enabled) {
        steal_pet_ticks();
    }
}

void xen::init_shared_info(uintptr_t shinfo_gpfn)
{
    using namespace ::intel_x64::msrs;

    m_shinfo = m_vcpu->map_gpa_4k<struct shared_info>(shinfo_gpfn << 12);
    m_shinfo_gpfn = shinfo_gpfn;

    auto vti = this->vcpu_time();
    vti->flags |= XEN_PVCLOCK_TSC_STABLE_BIT;
    vti->tsc_shift = m_tsc_shift;
    vti->tsc_to_system_mul = m_tsc_mul;

    /* Set the wallclock from start-of-day info */
    auto sod = m_dom->sod_info();
    auto now = ::x64::read_tsc::get();
    auto wc_nsec = tsc_to_ns(now - sod->tsc, m_tsc_shift,  m_tsc_mul);
    auto wc_sec = wc_nsec / 1000000000ULL;

    wc_nsec += sod->wc_nsec;
    wc_sec += sod->wc_sec;
    m_shinfo->wc_nsec = gsl::narrow_cast<uint32_t>(wc_nsec);
    m_shinfo->wc_sec = gsl::narrow_cast<uint32_t>(wc_sec);
    m_shinfo->wc_sec_hi = gsl::narrow_cast<uint32_t>(wc_sec >> 32);
    vti->tsc_timestamp = now;

    m_vcpu->add_resume_delegate({&xen::resume_update, this});
}

bool xen::vmexit_save_tsc(base_vcpu *vcpu)
{
    bfignored(vcpu);

    if (m_pet_enabled) {
        m_tsc_at_exit = ::x64::read_tsc::get();
    }

    return true;
}

bool xen::handle_pet(base_vcpu *vcpu)
{
    this->stop_timer();
    m_evtchn->queue_virq(VIRQ_TIMER);

    return true;
}

bool xen::handle_interrupt(base_vcpu *vcpu, interrupt_handler::info_t &info)
{
    auto parent = m_vcpu->parent_vcpu();
    auto guest_msi = parent->find_guest_msi(info.vector);

    if (guest_msi) {
        auto pdev = guest_msi->dev();
        expects(pdev);

        auto guest = get_guest(pdev->m_guest_vcpuid);
        if (!guest) {
            return true;
        }

        if (guest == m_vcpu) {
            guest->queue_external_interrupt(guest_msi->vector());
        } else {
            guest->push_external_interrupt(guest_msi->vector());
        }

        put_guest(pdev->m_guest_vcpuid);
    } else {
        m_vcpu->save_xstate();
        this->update_runstate(RUNSTATE_runnable);

        parent->load();
        parent->queue_external_interrupt(info.vector);
        parent->return_resume_after_interrupt();
    }

    return true;
}

bool xen::handle_hlt(
    base_vcpu *vcpu,
    bfvmm::intel_x64::hlt_handler::info_t &info)
{
    bfignored(vcpu);
    bfignored(info);

    using namespace vmcs_n;

    if (guest_rflags::interrupt_enable_flag::is_disabled()) {
        return false;
    }

    m_vcpu->advance();
    m_evtchn->queue_virq(VIRQ_TIMER);
    this->update_runstate(RUNSTATE_blocked);
    guest_interruptibility_state::blocking_by_sti::disable();

    auto pet = m_vcpu->get_preemption_timer();
    auto yield = ((pet << m_pet_shift) * 1000) / m_tsc_khz;

    m_vcpu->save_xstate();
    m_vcpu->parent_vcpu()->load();
    m_vcpu->parent_vcpu()->return_yield(yield);

    // unreachable
    return true;
}

bool xen::hypercall(xen_vcpu *vcpu)
{
    if (vcpu->rax() != __HYPERVISOR_console_io &&
        !(vcpu->rax() == __HYPERVISOR_vcpu_op &&
          vcpu->rdi() == VCPUOP_set_singleshot_timer) && !m_dom->ndvm()) {
        if (vcpu->rdi() > (1UL << 32)) {
            /* likely an address in rdi */
            printf("xen: hypercall %lu:0x%lx\n", vcpu->rax(), vcpu->rdi());
        } else {
            printf("xen: hypercall %lu:%lu\n", vcpu->rax(), vcpu->rdi());
        }
    }

    switch (vcpu->rax()) {
    case __HYPERVISOR_memory_op:
        return this->handle_memory_op();
    case __HYPERVISOR_xen_version:
        return this->handle_xen_version();
    case __HYPERVISOR_hvm_op:
        return this->handle_hvm_op();
    case __HYPERVISOR_event_channel_op:
        return this->handle_event_channel_op();
    case __HYPERVISOR_grant_table_op:
        return this->handle_grant_table_op();
    case __HYPERVISOR_platform_op:
        return this->handle_platform_op();
    case __HYPERVISOR_console_io:
        return this->handle_console_io();
    case __HYPERVISOR_sysctl:
        return this->handle_sysctl();
    case __HYPERVISOR_domctl:
        return this->handle_domctl();
    case __HYPERVISOR_xsm_op:
        return this->handle_xsm_op();
    case __HYPERVISOR_physdev_op:
        return this->handle_physdev_op();
    case __HYPERVISOR_vcpu_op:
        return this->handle_vcpu_op();
    case __HYPERVISOR_vm_assist:
        return this->handle_vm_assist();
    default:
        return false;
    }
}

xen::xen(xen_vcpu *vcpu, xen_domain *dom) :
    m_vcpu{vcpu},
    m_dom{dom},
    m_domctl{std::make_unique<class domctl>(this)},
    m_evtchn{std::make_unique<class evtchn>(this)},
    m_gnttab{std::make_unique<class gnttab>(this)},
    m_physdev{std::make_unique<class physdev>(this)},
    m_xenmem{std::make_unique<class xenmem>(this)},
    m_xenver{std::make_unique<class xenver>(this)},
    m_sysctl{std::make_unique<class sysctl>(this)}
{
    make_xen_ids(dom, this);

    m_tsc_khz = vcpu->m_yield_handler.m_tsc_freq;
    m_tsc_mul = (1000000000ULL << 32) / m_tsc_khz;
    m_tsc_shift = 0;
    m_pet_shift = vcpu->m_yield_handler.m_pet_shift;

    srand(dom->id());
    for (auto i = 0; i < sizeof(xdh); i++) {
        xdh[i] = rand() & 0xFF;
    }

    vcpu->add_cpuid_emulator(xen_leaf(0), {xen_leaf0});
    vcpu->add_cpuid_emulator(xen_leaf(2), {xen_leaf2});
    vcpu->emulate_wrmsr(hcall_page_msr, {wrmsr_hcall_page});
    vcpu->add_vmcall_handler({&xen::hypercall, this});
    vcpu->add_cpuid_emulator(xen_leaf(1), {xen_leaf1});
    vcpu->add_cpuid_emulator(xen_leaf(4), {&xen::xen_leaf4, this});

    vcpu->add_handler(0, handle_exception);
    vcpu->emulate_wrmsr(self_ipi_msr, {wrmsr_self_ipi});
}
}
