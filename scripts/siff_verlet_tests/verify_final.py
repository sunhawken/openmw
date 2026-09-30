from run_tests import *
import itertools,hashlib
LIBRARY='final.so'
REPORT_ROOT=ROOT

SETTINGS={'siff_hair':[4,.35,.94,6,1,.14,.30,1800,3.5,5,1500,.90],
          'siff_regalia':[3,.25,.91,6,.95,.24,.22,1800,2.5,5,1500,.80]}

def trailing_response(a, speed, deg):
    # Causal check: identical motion with drag enabled versus disabled. Root
    # acceleration inertia remains enabled in both runs. Verify steady pull,
    # direction, speed response and release after stopping.
    u=np.array([math.sin(math.radians(deg)),math.cos(math.radians(deg)),0.])
    snapshots=[]
    for drag in [0, SETTINGS[a.name][9]]:
        settings=SETTINGS[a.name].copy();settings[9]=drag;r=Rig(a,LIBRARY,settings);sample=[]
        for f in range(421):
            t=f/60;p=u*speed*np.clip(t-1,0,3);r.set(0,matrix(p));r.tick(t)
            if f in [180,420]:sample.append(r.positions[:,4:]-p)
        r.close();snapshots.append(sample)
    cruise=snapshots[1][0]-snapshots[0][0];settled=snapshots[1][1]-snapshots[0][1]
    return {'speed':speed,'direction_deg':deg,'steady_drag_trail_units':float(-(cruise@u).mean()),
            'stopped_drag_offset_units':float(np.linalg.norm(settled.mean((0,1)))),
            'steady_drag_tip_trail_units':float(-(cruise[:,-1]@u).mean())}

def full_speed_lift(a,deg):
    c=run(a,LIBRARY,'full_run',math.radians(deg),settings=SETTINGS[a.name],record=True)
    u=np.array([math.sin(math.radians(deg)),math.cos(math.radians(deg)),0.])
    root_angles=[];tip_lifts=[];tip_trails=[]
    for index in range(54,175):
        world=c['worlds'][index]
        p=(np.concatenate([c['history'][index],np.ones((*a.rest.shape[:2],1))],axis=2)@np.linalg.inv(world[0]))[:,:,:3]
        d=p[:,3]-p[:,2]
        root_angles.append(np.degrees(np.arctan2(-(d@u),-d[:,2])))
        tip_lifts.append((p[:,-1]-a.rest[:,-1])[:,2].mean())
        tip_trails.append(-((p[:,-1]-a.rest[:,-1])@u).mean())
    poses=c['worlds'][[54,90,120,174]]
    c.pop('worlds');c.pop('history')
    return {**c,'minimum_median_free_root_tilt_degrees':float(np.median(root_angles,axis=1).min()),
            'minimum_individual_free_root_tilt_degrees':float(np.min(root_angles)),
            'minimum_mean_tip_lift_units':float(min(tip_lifts)),
            'minimum_mean_tip_trail_units':float(min(tip_trails)),
            'speed_units_per_second':320,'measured_cruise_interval_seconds':[1.8,5.8]},poses

def extras(a):
    # Uneven timesteps, explicit frame stalls, backwards clock and teleport.
    r=Rig(a,LIBRARY,SETTINGS[a.name]);r.tick(0);t=0;worst=1.;width=1.;last=[]
    lengths=np.linalg.norm(np.diff(a.rest,axis=1),axis=2)
    sv=[np.linalg.svd(a.rest[:,i]-a.rest[:,i].mean(0),compute_uv=False)[1] for i in range(3,a.rest.shape[1])]
    for i in range(720):
        dt=[1/30,1/144,1/60,1/120][i%4]
        if i in [130,230,330]:dt=.12
        if i==430:dt=.35
        t+=dt
        p=np.array([20*math.sin(t*3),18*math.cos(t*4),max(0,45*math.sin(t*2))]) if i<480 else np.zeros(3)
        r.set(0,matrix(p,angles=(0,0,t*1.5 if i<480 else 0)));animate(r,t*9,float(i<480));r.tick(t)
        v=r.positions.astype(float)
        if not np.isfinite(v).all():raise RuntimeError('nonfinite timing test')
        worst=max(worst,float((np.linalg.norm(np.diff(v,axis=1),axis=2)[:,2:]/lengths[:,2:]).max()))
        for k,j in enumerate(range(3,v.shape[1])):width=min(width,float(np.linalg.svd(v[:,j]-v[:,j].mean(0),compute_uv=False)[1]/sv[k]))
        if i>660:last.append(v.copy())
    pause_settle=float(np.sqrt(np.mean(np.diff(last,axis=0)**2))*60)
    # Teleport/reset must be exactly rest, with no stored momentum.
    r.set(0,matrix((500,350,80)));r.tick(t+.016)
    rest=a.rest+np.array([500,350,80]);err=float(np.abs(r.positions-rest).max())
    r.tick(t-.5);rewind_err=float(np.abs(r.positions-rest).max())
    # Long quiet settle, then verify wake on renewed movement/collider animation.
    for i in range(1800):r.tick(t+1+i/60)
    lastp=r.positions.copy()
    motions=[]
    for i in range(60):r.tick(t+31+i/60);motions.append(r.positions.copy())
    settle=float(np.sqrt(np.mean(np.diff(motions,axis=0)**2))*60)
    sleep=np.zeros(12,np.int32);r.lib.rig_sleep.argtypes=[C.c_void_p,C.c_void_p];r.lib.rig_sleep(r.ptr,sleep.ctypes.data)
    r.set(0,matrix((502,350,80)));animate(r,2,1);r.tick(t+32)
    r.lib.rig_sleep(r.ptr,sleep.ctypes.data);wake=bool((sleep==0).all())
    r.close()
    return {'case':'variable_dt_pause_teleport_rewind_settle_wake','max_segment_ratio':worst,'min_ring_thickness_ratio':width,'pause_tail_rms':pause_settle,'teleport_max_error':err,'rewind_max_error':rewind_err,'long_settle_rms_units_per_s':settle,'wake_pass':wake}

def mesh_metrics(a,worlds):
    # Actual full-resolution skinning; group vertices by weighted chain level.
    allrest=[];masks=[];allframes=[];vertex_count=0;normerr=0;maxinfluence=0;resterr=0
    for i,s in enumerate(a.meta['shapes']):
        w=a.d[f'w{i}'];bi=a.d[f'bi{i}'];levels={int(n):j for c in a.chain for j,n in enumerate(c)}
        li=np.array([levels.get(int(n),-1) for n in bi]);chainw=w[:,li>=0].sum(1)
        level=(w[:,li>=0]@li[li>=0])/np.maximum(chainw,1e-12)
        mask=(chainw>.50)&(level>=3)
        ref=a.skin(a.d['world'],i);resterr=max(resterr,float(np.abs(ref-a.d[f'ref{i}']).max()))
        skininv=np.linalg.inv(a.d[f'skin{i}']);root=s['root']
        # Common skeleton coordinates, with actor motion removed.
        ref=(np.column_stack([ref,np.ones(len(ref))])@skininv@a.d['world'][root])[:,:3]
        allrest.append(ref);masks.append((mask,np.clip(np.rint(level).astype(int),0,a.chain.shape[1]-1)))
        vertices=[]
        for world in worlds:
            v=a.skin(world,i)
            v=(np.column_stack([v,np.ones(len(v))])@skininv@world[root]@np.linalg.inv(world[0]))[:,:3]
            vertices.append(v)
        allframes.append(np.array(vertices));vertex_count+=len(w);normerr=max(normerr,float(np.abs(w.sum(1)-1).max()));maxinfluence=max(maxinfluence,int((w>1e-7).sum(1).max()))
    rest=np.concatenate(allrest);frames=np.concatenate(allframes,axis=1);mask=np.concatenate([x[0] for x in masks]);levels=np.concatenate([x[1] for x in masks])
    min_width=1.;min_area=1.;worst={}
    for lv in range(3,a.chain.shape[1]):
        which=mask&(levels==lv)
        if which.sum()<100:continue
        base=rest[which];base_sv=np.linalg.svd(base-base.mean(0),compute_uv=False)
        if base_sv[1]<1e-4:continue
        for frame_index,f in enumerate(frames):
            v=f[which];sv=np.linalg.svd(v-v.mean(0),compute_uv=False)
            ratio=float(sv[1]/base_sv[1])
            if ratio<min_width:worst={'combined_sample_index':frame_index,'chain_level':lv,'width_ratio':ratio}
            min_width=min(min_width,ratio);min_area=min(min_area,float((sv[0]*sv[1])/(base_sv[0]*base_sv[1])))
    return {'checked_vertices':vertex_count,'checked_mesh_frames':len(worlds),'min_surface_band_width_ratio':min_width,'min_surface_band_area_ratio':min_area,'worst_surface_band_sample':worst,'rest_skin_evaluator_max_error':resterr,'max_weight_sum_error':normerr,'max_influences':maxinfluence}

def validate(report):
    failures=[]
    for n,a in report['assets'].items():
        # Bone centerlines are diagnostics, not the skinned surface: their rings
        # can bunch against a collider while vertex offsets retain mesh volume.
        if any(c['max_segment_ratio']>1.05 for c in a['cases']):failures.append(n+' chain lengths')
        if a['mesh']['min_surface_band_width_ratio']<.65:failures.append(n+' mesh width')
        if a['extra']['teleport_max_error']>.001 or a['extra']['rewind_max_error']>.001 or not a['extra']['wake_pass']:failures.append(n+' reset/wake')
        if a['extra']['long_settle_rms_units_per_s']>.10:failures.append(n+' long settle')
        if any(t['steady_drag_trail_units'] < .75 or t['stopped_drag_offset_units'] > 1.5 for t in a['trailing_response']):failures.append(n+' trailing/release')
        for i in range(8):
            if a['trailing_response'][i+8]['steady_drag_trail_units'] < 1.3*a['trailing_response'][i]['steady_drag_trail_units']:failures.append(n+' speed response')
        limits=(45,25,40) if n=='siff_hair' else (35,15,25)
        for c in a['full_speed_lift']:
            if c['minimum_median_free_root_tilt_degrees']<limits[0] or c['minimum_mean_tip_lift_units']<limits[1] or c['minimum_mean_tip_trail_units']<limits[2]:
                failures.append(n+' full-speed root lift '+str(c['direction_deg']))
    return failures


def main():
    report={'scope':'Headless execution of actual controller C++; minimal scene/matrix adapter; procedural actor-axis gait; not OpenMW gameplay.','settings':SETTINGS,'assets':{}}
    source_root=ROOT.parents[1]
    report['source_sha256']={str(p.relative_to(source_root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in
        [source_root/'apps/openmw/mwrender'/n for n in ['verletclothcontroller.cpp','verletclothcontroller.hpp','animation.cpp']]+[ROOT/'bridge.cpp',ROOT/'shim.hpp']}
    report['compiled_adapter_sha256']=hashlib.sha256((ROOT/LIBRARY).read_bytes()).hexdigest()
    report['opt_in_settings']={'stable_timing':True,'project_velocity':True,'rest_collision_fit':True,'align_bones':True,'pin_count':3,'gravity':711,'iterations':32,'body_collision_radius':12,'body_collision_margin':1.5,'idle_wind':False}
    REPORT_ROOT.mkdir(parents=True,exist_ok=True)
    for name in SETTINGS:
        a=Asset(name);cases=[]
        sampled_worlds=[]
        for kind in ['walk','run','jump']:
            for deg in range(0,360,45):
                c=run(a,LIBRARY,kind,math.radians(deg),settings=SETTINGS[name],record=True)
                sampled_worlds.append(c.pop('worlds')[[10,54,90,135]])
                c.pop('history');cases.append(c)
            print(name,kind,'8 directions completed',flush=True)
        for u in itertools.product([-1,0,1],repeat=3):
            if u!=(0,0,0):
                c=run(a,LIBRARY,'pull',settings=SETTINGS[name],pull_vector=u);c['pull_direction']=list(u);cases.append(c)
        for fps in [20,30,60,120,144]:cases.append(run(a,LIBRARY,'extreme',fps=fps,settings=SETTINGS[name]))
        extra=extras(a)
        trailing=[trailing_response(a,speed,deg) for speed in [110,320] for deg in range(0,360,45)]
        lift=[]
        for deg in range(0,360,45):
            c,poses=full_speed_lift(a,deg);lift.append(c);sampled_worlds.append(poses)
        # Full mesh checks at snapshots across backward running/jumping/extremes.
        recordings=[]
        for kind in ['run','jump','extreme']:
            c=run(a,LIBRARY,kind,math.pi,record=True,settings=SETTINGS[name]);recordings.append(c['worlds'][::max(1,len(c['worlds'])//12)])
            np.savez_compressed(REPORT_ROOT/(name+'-'+kind+'-recording.npz'),history=c['history'],worlds=c['worlds'])
        mesh=mesh_metrics(a,np.concatenate(recordings+sampled_worlds))
        report['assets'][name]={'cases':cases,'extra':extra,'trailing_response':trailing,'full_speed_lift':lift,'mesh':mesh,'summary':{'case_count':len(cases)+1+len(trailing)+len(lift),'max_segment_stretch_percent':100*(max(x['max_segment_ratio'] for x in cases+[extra]+lift)-1),'min_chain_band_width_percent':100*min(x['min_ring_thickness_ratio'] for x in cases+[extra]+lift),'min_mesh_band_width_percent':100*mesh['min_surface_band_width_ratio'],'settle_speed':extra['long_settle_rms_units_per_s'], 'min_running_trail_units':min(x['steady_drag_trail_units'] for x in trailing if x['speed']==320),
            'min_full_speed_median_root_tilt_degrees':min(x['minimum_median_free_root_tilt_degrees'] for x in lift)}}
        print(name,report['assets'][name]['summary'],'extra',extra,'mesh',mesh,flush=True)
        (REPORT_ROOT/'Directional-Test-Report.json').write_text(json.dumps(report,indent=2))
    report['acceptance']={'segment_ratio_max':1.05,'surface_band_width_ratio_min':.65,'stopped_drag_offset_max_units':1.5,'running_to_walking_trail_min_ratio':1.3,'note':'Bone-center ring widths are retained as diagnostics. Mesh volume is assessed on full-resolution skinned surface bands because centerlines are not surface vertices.'}
    report['acceptance']['full_speed_root_lift']={'hair':{'median_tilt_min_degrees':45,'mean_tip_lift_min_units':25,'mean_tip_trail_min_units':40},'skirt':{'median_tilt_min_degrees':35,'mean_tip_lift_min_units':15,'mean_tip_trail_min_units':25},'scope':'Every compass direction, over the complete 1.8–5.8 s continuous cruise at 320 units/s.'}
    report['failures']=validate(report);report['passed']=not report['failures']
    (REPORT_ROOT/'Directional-Test-Report.json').write_text(json.dumps(report,indent=2));print('PASS' if report['passed'] else 'FAIL',report['failures'],flush=True)

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser();parser.add_argument('--library',default='final.so');parser.add_argument('--output-dir',type=Path,default=ROOT)
    args=parser.parse_args();LIBRARY=args.library;REPORT_ROOT=args.output_dir.resolve();main()
