# Cutscene camera evenness (night of 31 Oct): for each pdz_cutscene_frame_dump CSV, the share
# of moving frames whose view step is under 0.4x or over 1.6x the median of its 16 neighbours, and how many of those
# sit at a camera block switch.   python -I tools/perf/cutscene_evenness.py <dump.csv> [...]
import csv, sys, math, statistics
for fn in sys.argv[1:]:
    rows=[r for r in csv.DictReader(open(fn)) if r['phase']=='cutscene' and r['view_eye_x']]
    def eye(r): return (float(r['view_eye_x']),float(r['view_eye_y']),float(r['view_eye_z']))
    def tgt(r): return (float(r['view_target_x']),float(r['view_target_y']),float(r['view_target_z']))
    st=[0.0]+[math.dist(eye(rows[i]),eye(rows[i-1]))+0.2*math.dist(tgt(rows[i]),tgt(rows[i-1])) for i in range(1,len(rows))]
    jerk=0; sw=0; jerk_sw=0; ratios=[]
    for i in range(8,len(rows)-8):
        nb=sorted(st[i-8:i]+st[i+1:i+9]); m=nb[8]
        if m<0.5: continue
        ratio=st[i]/m; ratios.append(ratio)
        is_sw = rows[i]['block']!=rows[i-1]['block']
        sw+=is_sw
        if ratio>1.6 or ratio<0.4:
            jerk+=1; jerk_sw+=is_sw
    ratios.sort()
    print(f"{fn}: moving frames {len(ratios)}, uneven (step <0.4x or >1.6x local median) {jerk} ({100*jerk/max(1,len(ratios)):.1f}%), of them at a block switch {jerk_sw} of {sw} switches; p5/p95 ratio {ratios[len(ratios)//20]:.2f}/{ratios[-len(ratios)//20]:.2f}")
