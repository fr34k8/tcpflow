/**
 * one_page_report.cpp: 
 * Generate a one-page visualization from TCP packets
 *
 * This source file is public domain, as it is not based on the original tcpflow.
 *
 * Author: Michael Shick <mike@shick.in>
 *
 */

#include "config.h"

#include "plot_view.h"
#ifdef HAVE_LIBCAIRO
#include "tcpflow.h"
#include "tcpip.h"

#include <ctime>
#include <iomanip>
#include <math.h>

#include "one_page_report.h"

using namespace std;

// string constants
const string one_page_report::title_version = PACKAGE_NAME " " PACKAGE_VERSION;
const vector<string> one_page_report::size_suffixes =
        one_page_report::build_size_suffixes();
// ratio constants
const double one_page_report::page_margin_factor = 0.05;
const double one_page_report::line_space_factor = 0.25;
const double one_page_report::histogram_pad_factor_y = 1.0;
const double one_page_report::address_histogram_width_divisor = 2.5;
// size constants
const double one_page_report::packet_histogram_height = 100.0;
const double one_page_report::address_histogram_height = 125.0;
const double one_page_report::port_histogram_height = 100.0;
// color constants
const plot_view::rgb_t one_page_report::default_color(0.67, 0.67, 0.67);

one_page_report::one_page_report() : 
    source_identifier(), filename("report.pdf"),
    bounds(0.0, 0.0, 611.0, 792.0), header_font_size(8.0),
    top_list_font_size(8.0), histogram_show_top_n_text(3),
    packet_count(0), byte_count(0), earliest(), latest(),
    transport_counts(), packet_histogram(), src_port_histogram(),
    dst_port_histogram(), pfall(), netmap(), src_tree(), dst_tree(),
    port_aliases(), port_color_map()
{
    earliest = (struct timeval) { 0 };
    latest = (struct timeval) { 0 };

    port_color_map[PORT_HTTP] = plot_view::rgb_t(0.07, 0.44, 0.87);
    port_color_map[PORT_HTTPS] = plot_view::rgb_t(0.25, 0.79, 0.40);

    // build null alias map to avoid requiring special handling for unmapped ports
    for(int ii = 0; ii <= 65535; ii++) {
        port_aliases[ii] = ii;
    }
}

void one_page_report::ingest_packet(const be13::packet_info &pi)
{
    if(earliest.tv_sec == 0) {
        earliest = pi.ts;
    }
    if(pi.ts.tv_sec > latest.tv_sec && pi.ts.tv_usec > latest.tv_usec) {
        latest = pi.ts;
    }

    packet_count++;
    byte_count += pi.pcap_hdr->caplen;
    transport_counts[pi.ether_type()]++; // should we handle VLANs?

    // break out TCP/IP info and feed child views

    // feed IP-only views
    uint8_t ip_ver = 0;
    if(pi.is_ip4()) {
        ip_ver = 4;

        src_tree.add((uint8_t *) pi.ip_data + pi.ip4_src_off, 4, pi.ip_datalen);
        dst_tree.add((uint8_t *) pi.ip_data + pi.ip4_dst_off, 4, pi.ip_datalen);
    }
    else if(pi.is_ip6()) {
        ip_ver = 6;

        src_tree.add((uint8_t *) pi.ip_data + pi.ip6_src_off, 16, pi.ip_datalen);
        dst_tree.add((uint8_t *) pi.ip_data + pi.ip6_dst_off, 16, pi.ip_datalen);
    }
    else {
        return;
    }


    // feed TCP views
    uint16_t tcp_src = 0, tcp_dst = 0;

    switch(ip_ver) {
        case 4:
            if(!pi.is_ip4_tcp()) {
                return;
            }
            tcp_src = pi.get_ip4_tcp_sport();
            tcp_dst = pi.get_ip4_tcp_dport();
            break;
        case 6:
            if(!pi.is_ip6_tcp()) {
                return;
            }
            tcp_src = pi.get_ip6_tcp_sport();
            tcp_dst = pi.get_ip6_tcp_dport();
            break;
        default:
            return;
    }

    packet_histogram.insert(pi.ts, tcp_src);
    src_port_histogram.increment(tcp_src, pi.ip_datalen);
    dst_port_histogram.increment(tcp_dst, pi.ip_datalen);
}

void one_page_report::render(const string &outdir)
{
    cairo_t *cr;
    cairo_surface_t *surface;
    string fname = outdir + "/" + filename;

    surface = cairo_pdf_surface_create(fname.c_str(),
				 bounds.width,
				 bounds.height);
    cr = cairo_create(surface);

    double pad_size = bounds.width * page_margin_factor;
    plot_view::bounds_t pad_bounds(bounds.x + pad_size,
            bounds.y + pad_size, bounds.width - pad_size * 2,
            bounds.height - pad_size * 2);
    cairo_translate(cr, pad_bounds.x, pad_bounds.y);

    render_pass pass(*this, cr, pad_bounds);

    pass.render_header();
    
    // time histogram
    time_histogram_view th_view(packet_histogram, port_color_map, default_color);
    th_view.title = "TCP Packets Received";
    th_view.pad_left_factor = 0.2;
    th_view.y_tick_font_size = 6.0;
    th_view.x_tick_font_size = 6.0;
    th_view.x_axis_font_size = 8.0;
    th_view.x_axis_decoration = plot_view::AXIS_SPAN_ARROW;
    pass.render(th_view);

    if(getenv("DEBUG")) {
    pass.render_map();

    pass.render_packetfall();
    }

    // address histograms
    // histograms are built from iptree here
    address_histogram src_addr_histogram(src_tree);
    address_histogram dst_addr_histogram(dst_tree);
    address_histogram_view src_ah_view(src_addr_histogram);
    if(src_addr_histogram.size() > 0) {
        src_ah_view.title = "Top Source Addresses";
    }
    else {
        src_ah_view.title = "No Source Addresses";
    }
    src_ah_view.bar_color = default_color;
    address_histogram_view dst_ah_view(dst_addr_histogram);
    if(dst_addr_histogram.size() > 0) {
        dst_ah_view.title = "Top Destination Addresses";
    }
    else {
        dst_ah_view.title = "No Destination Addresses";
    }
    dst_ah_view.bar_color = default_color;
    pass.render(src_ah_view, dst_ah_view);

    // address histograms
    port_histogram_view sp_view(src_port_histogram, port_color_map, default_color);
    port_histogram_view dp_view(dst_port_histogram, port_color_map, default_color);
    if(src_port_histogram.size()) {
        sp_view.title = "Top Source Ports";
    }
    else {
        sp_view.title = "No Source Ports";
    }
    if(dst_port_histogram.size()) {
        dp_view.title = "Top Destination Ports";
    }
    else {
        dp_view.title = "No Destination Ports";
    }
    pass.render(sp_view, dp_view);

    // cleanup
    cairo_destroy (cr);
    cairo_surface_destroy(surface);
}

string one_page_report::pretty_byte_total(uint64_t byte_count)
{
    //// packet count/size
    uint64_t size_log_1000 = (uint64_t) (log(byte_count) / log(1000));
    if(size_log_1000 >= size_suffixes.size()) {
        size_log_1000 = 0;
    }
    return ssprintf("%.2f %s", (double) byte_count / pow(1000.0, (double) size_log_1000),
            size_suffixes.at(size_log_1000).c_str());
}

void one_page_report::render_pass::render_header()
{
    string formatted;
    // title
    double title_line_space = report.header_font_size * line_space_factor;
    //// version
    render_text_line(title_version, report.header_font_size,
            title_line_space);
    //// input
    formatted = ssprintf("Input: %s", report.source_identifier.c_str());
    render_text_line(formatted.c_str(), report.header_font_size,
            title_line_space);
    //// date generated
    time_t gen_unix = time(0);
    struct tm gen_time = *localtime(&gen_unix);
    formatted = ssprintf("Generated: %04d-%02d-%02d %02d:%02d:%02d",
            1900 + gen_time.tm_year, 1 + gen_time.tm_mon, gen_time.tm_mday,
            gen_time.tm_hour, gen_time.tm_min, gen_time.tm_sec);
    render_text_line(formatted.c_str(), report.header_font_size,
            title_line_space);
    //// trailing pad
    end_of_content += title_line_space * 4;
    // quick stats
    //// date range
    struct tm start = *localtime(&report.earliest.tv_sec);
    struct tm stop = *localtime(&report.latest.tv_sec);
    formatted = ssprintf("Date range: %04d-%02d-%02d %02d:%02d:%02d -- %04d-%02d-%02d %02d:%02d:%02d",
            1900 + start.tm_year, 1 + start.tm_mon, start.tm_mday,
            start.tm_hour, start.tm_min, start.tm_sec,
            1900 + stop.tm_year, 1 + stop.tm_mon, stop.tm_mday,
            stop.tm_hour, stop.tm_min, stop.tm_sec);
    render_text_line(formatted.c_str(), report.header_font_size,
            title_line_space);
    //// packet count/size
    formatted = ssprintf("Packets analyzed: %s (%sB)",
            comma_number_string(report.packet_count).c_str(),
            pretty_byte_total(report.byte_count).c_str());
    render_text_line(formatted.c_str(), report.header_font_size,
            title_line_space);
    //// protocol breakdown
    uint64_t transport_total = 0;
    for(map<uint32_t, uint64_t>::const_iterator ii =
                report.transport_counts.begin();
            ii != report.transport_counts.end(); ii++) {
        transport_total += ii->second;
    }

    formatted = ssprintf("Transports: IPv4 %.2f%% IPv6 %.2f%% ARP %.2f%% Other %.2f%%",
            ((double) report.transport_counts[ETHERTYPE_IP] / (double) transport_total) * 100.0,
            ((double) report.transport_counts[ETHERTYPE_IPV6] / (double) transport_total) * 100.0,
            ((double) report.transport_counts[ETHERTYPE_ARP] / (double) transport_total) * 100.0,
            (1.0 - ((double) (report.transport_counts[ETHERTYPE_IP] +
                              report.transport_counts[ETHERTYPE_IPV6] +
                              report.transport_counts[ETHERTYPE_ARP]) /
                    (double) transport_total)) * 100.0);
    render_text_line(formatted.c_str(), report.header_font_size,
            title_line_space);
    // trailing pad for entire header
    end_of_content += title_line_space * 4;
}

void one_page_report::render_pass::render_text(string text,
        double font_size, double x_offset,
        cairo_text_extents_t &rendered_extents)
{
    cairo_set_font_size(surface, font_size);
    cairo_set_source_rgb(surface, 0.0, 0.0, 0.0);
    cairo_text_extents(surface, text.c_str(), &rendered_extents);
    cairo_move_to(surface, x_offset, end_of_content + rendered_extents.height);
    cairo_show_text(surface, text.c_str());
}

void one_page_report::render_pass::render_text_line(string text,
        double font_size, double line_space)
{
    cairo_text_extents_t extents;
    render_text(text, font_size, 0.0, extents);
    end_of_content += extents.height + line_space;
}

void one_page_report::render_pass::render(time_histogram_view &view)
{
    plot_view::bounds_t bounds(0.0, end_of_content, surface_bounds.width,
            packet_histogram_height);

    view.render(surface, bounds);

    end_of_content += bounds.height * histogram_pad_factor_y;
}

void one_page_report::render_pass::render_packetfall()
{
    plot_view::bounds_t bounds(0.0, end_of_content, surface_bounds.width,
            packet_histogram_height);

    report.pfall.render(surface, bounds);

    end_of_content += bounds.height * histogram_pad_factor_y;
}

void one_page_report::render_pass::render_map()
{
    plot_view::bounds_t bounds(0.0, end_of_content, surface_bounds.width,
            packet_histogram_height);

    report.netmap.render(surface, bounds);

    end_of_content += bounds.height * histogram_pad_factor_y;
}

void one_page_report::render_pass::render(address_histogram_view &left, address_histogram_view &right)
{
    double width = surface_bounds.width / address_histogram_width_divisor;
    const address_histogram &left_data = left.get_data();
    const address_histogram &right_data = right.get_data();
    uint64_t total_datagrams = left_data.ingest_count();

    plot_view::bounds_t left_bounds(0.0, end_of_content, width, address_histogram_height);
    left.render(surface, left_bounds);

    plot_view::bounds_t right_bounds(surface_bounds.width - width, end_of_content,
            width, address_histogram_height);
    right.render(surface, right_bounds);

    end_of_content += max(left_bounds.height, right_bounds.height);

    // text stats
    string stat_line_format = "%d) %s - %sB (%d%%)";
    for(size_t ii = 0; ii < report.histogram_show_top_n_text; ii++) {
        cairo_text_extents_t left_extents, right_extents;

        if(left_data.size() > ii && left_data.at(ii).count > 0) {
            const iptree::addr_elem &addr = left_data.at(ii);
            uint8_t percentage = 0;

            percentage = (uint8_t) (((double) addr.count / (double) total_datagrams) * 100.0);

            string str = ssprintf(stat_line_format.c_str(), ii + 1, addr.str().c_str(),
                    pretty_byte_total(addr.count).c_str(), percentage);

            render_text(str.c_str(), report.top_list_font_size, left_bounds.x,
                    left_extents);
        }

        if(right_data.size() > ii && right_data.at(ii).count > 0) {
            const iptree::addr_elem &addr = right_data.at(ii);
            uint8_t percentage = 0;

            percentage = (uint8_t) (((double) addr.count / (double) total_datagrams) * 100.0);

            string str = ssprintf(stat_line_format.c_str(), ii + 1, addr.str().c_str(),
                    pretty_byte_total(addr.count).c_str(), percentage);

            render_text(str.c_str(), report.top_list_font_size, right_bounds.x,
                    right_extents);
        }

        if((left_data.size() > ii && left_data.at(ii).count > 0) ||
                (right_data.size() > ii && right_data.at(ii).count > 0)) {
            end_of_content += max(left_extents.height, right_extents.height) * 1.5;
        }
    }

    end_of_content += max(left_bounds.height, right_bounds.height) *
        (histogram_pad_factor_y - 1.0);
}

void one_page_report::render_pass::render(port_histogram_view &left, port_histogram_view &right)
{
    port_histogram &left_data = left.get_data();
    port_histogram &right_data = right.get_data();

    uint64_t total_bytes = left_data.ingest_count();

    double width = surface_bounds.width / address_histogram_width_divisor;

    plot_view::bounds_t left_bounds(0.0, end_of_content, width, port_histogram_height);
    left.render(surface, left_bounds);

    plot_view::bounds_t right_bounds(surface_bounds.width - width, end_of_content,
            width, port_histogram_height);
    right.render(surface, right_bounds);

    end_of_content += max(left_bounds.height, right_bounds.height);

    // text stats
    string stat_line_format = "%d) %d - %sB (%d%%)";
    for(size_t ii = 0; ii < report.histogram_show_top_n_text; ii++) {
        cairo_text_extents_t left_extents, right_extents;

        if(left_data.size() > ii && left_data.at(ii).count > 0) {
            port_histogram::port_count port = left_data.at(ii);
            uint8_t percentage = 0;

            percentage = (uint8_t) (((double) port.count / (double) total_bytes) * 100.0);

            string str = ssprintf(stat_line_format.c_str(), ii + 1, port.port,
                    pretty_byte_total(port.count).c_str(), percentage);

            render_text(str.c_str(), report.top_list_font_size, left_bounds.x,
                    left_extents);
        }

        if(right_data.size() > ii && right_data.at(ii).count > 0) {
            port_histogram::port_count port = right_data.at(ii);
            uint8_t percentage = 0;

            percentage = (uint8_t) (((double) port.count / (double) total_bytes) * 100.0);

            string str = ssprintf(stat_line_format.c_str(), ii + 1, port.port,
                    pretty_byte_total(port.count).c_str(), percentage);

            render_text(str.c_str(), report.top_list_font_size, right_bounds.x,
                    right_extents);
        }

        if((left_data.size() > ii && left_data.at(ii).count > 0) ||
                (right_data.size() > ii && right_data.at(ii).count > 0)) {
            end_of_content += max(left_extents.height, right_extents.height) * 1.5;
        }
    }

    end_of_content += max(left_bounds.height, right_bounds.height) *
        (histogram_pad_factor_y - 1.0);
}


vector<string> one_page_report::build_size_suffixes()
{
    vector<string> v;
    v.push_back("");
    v.push_back("K");
    v.push_back("M");
    v.push_back("G");
    v.push_back("T");
    v.push_back("P");
    v.push_back("E");
    return v;
}
#endif
