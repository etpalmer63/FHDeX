subroutine get_ptsource_2d( lo, hi, &
     &            iface, if_lo, if_hi, &
     &            ptS, pts_lo, pts_hi, &
     &            strength, dx, ib_cen_x, &
     &            ib_cen_y, prob_lo ) bind(C, name="get_ptsource_2d")
  
  use amrex_mempool_module, only : bl_allocate, bl_deallocate

  implicit none

  double precision, intent(in) :: strength, prob_lo(2), dx(2)
  double precision, intent(in) :: ib_cen_x, ib_cen_y
  integer, intent(in) :: lo(2), hi(2)
  integer, intent(in) :: if_lo(2), if_hi(2)
  integer, intent(in) :: pts_lo(2), pts_hi(2)
  integer, intent(in) :: iface(if_lo(1):if_hi(1),if_lo(2):if_hi(2))
  double precision, intent(out) :: ptS(pts_lo(1):pts_hi(1),pts_lo(2):pts_hi(2))

  integer :: i, j
  double precision :: y


     do    j = lo(2), hi(2)
        do i = lo(1), hi(1)
           y = prob_lo(2)+(dble(j)+0.5d0)*dx(2)
           if ((iface(i,j).eq.1) .and. (y .le. ib_cen_y)) then
           pts(i,j)=strength
           endif
        enddo
     enddo

end subroutine get_ptsource_2d
